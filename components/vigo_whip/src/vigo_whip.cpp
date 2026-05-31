#include "vigo_whip.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "h264_encoder.hpp"
#include "lwip/sockets.h"
#include "mbedtls/debug.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/psa_util.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <sstream>

static const char *TAG = "VigWhip";

// static debug callback for mbedTLS
static void mbedtls_debug_cb(void *ctx, int level, const char *file, int line,
                             const char *str) {
  std::string s(str);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
    s.pop_back();
  }
  ESP_LOGI("mbedtls", "[%d, %s:%d] %s", level, file, line, s.c_str());
}

// Helper CRC32 implementation for STUN packet fingerprint validation
static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
  }
  return ~crc;
}

// Helper to append formatted attributes to STUN request payloads
static void append_stun_attr(std::vector<uint8_t> &buf, uint16_t type,
                             const uint8_t *val, uint16_t len) {
  buf.push_back((type >> 8) & 0xFF);
  buf.push_back(type & 0xFF);
  buf.push_back((len >> 8) & 0xFF);
  buf.push_back(len & 0xFF);
  if (len > 0 && val != nullptr) {
    buf.insert(buf.end(), val, val + len);
  }
  // Pad value attribute to 4-byte boundaries per RFC 5389
  while (buf.size() % 4 != 0) {
    buf.push_back(0x00);
  }
}

// Standard WebRTC SRTP Key Derivation Function (KDF) compliant with RFC 3711
// Section 4.3
static void srtp_kdf(const uint8_t *master_key, const uint8_t *master_salt,
                     uint8_t label, uint8_t *out, size_t out_len) {
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
  psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
  psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attributes, 128);

  psa_key_id_t key_id = 0;
  psa_status_t status = psa_import_key(&attributes, master_key, 16, &key_id);
  if (status != PSA_SUCCESS) {
    ESP_LOGE("srtp_kdf", "Failed to import KDF key: %d", (int)status);
    return;
  }

  size_t derived = 0;
  uint32_t index = 0;
  while (derived < out_len) {
    uint8_t x[16] = {0};
    memcpy(x, master_salt, 14);
    x[7] ^= label;
    x[13] ^= (index >> 16) & 0xFF;
    x[14] ^= (index >> 8) & 0xFF;
    x[15] ^= index & 0xFF;

    uint8_t keystream[16];
    size_t output_length = 0;
    status = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING, x, 16, keystream, 16,
                                &output_length);
    if (status != PSA_SUCCESS) {
      ESP_LOGE("srtp_kdf", "PSA encryption failed: %d", (int)status);
      break;
    }

    size_t chunk = std::min(out_len - derived, (size_t)16);
    memcpy(out + derived, keystream, chunk);
    derived += chunk;
    index++;
  }
  psa_destroy_key(key_id);
}

static psa_status_t psa_hmac_sha1(const uint8_t *key, size_t key_len,
                                  const uint8_t *input, size_t input_len,
                                  uint8_t *output, size_t output_size,
                                  size_t *output_len) {
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_1));
  psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);

  psa_key_id_t key_id;
  psa_status_t status = psa_import_key(&attributes, key, key_len, &key_id);
  if (status != PSA_SUCCESS) {
    return status;
  }

  status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_1), input, input_len,
                           output, output_size, output_len);

  psa_destroy_key(key_id);
  return status;
}

namespace vigo::whip {

static std::string unescape_pem(const std::string &input) {
  std::string output;
  output.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    if (i + 2 < input.length() && input[i] == '\\' && input[i + 1] == '\\' &&
        input[i + 2] == 'n') {
      output += '\n';
      i += 2;
    } else if (i + 1 < input.length() && input[i] == '\\' && input[i + 1] == 'n') {
      output += '\n';
      i++;
    } else if (i + 2 < input.length() && input[i] == '\\' && input[i + 1] == '\\' &&
               input[i + 2] == 'r') {
      output += '\r';
      i += 2;
    } else if (i + 1 < input.length() && input[i] == '\\' && input[i + 1] == 'r') {
      output += '\r';
      i++;
    } else {
      output += input[i];
    }
  }
  return output;
}

static bool is_private_ip(const std::string &ip) {
  if (ip.empty())
    return true;
  if (ip == "127.0.0.1" || ip == "0.0.0.0" || ip == "localhost")
    return true;

  if (ip.rfind("10.", 0) == 0)
    return true;
  if (ip.rfind("192.168.", 0) == 0)
    return true;

  if (ip.rfind("172.", 0) == 0) {
    size_t dot = ip.find('.', 4);
    if (dot != std::string::npos) {
      std::string s_octet2 = ip.substr(4, dot - 4);
      bool all_digits = !s_octet2.empty();
      for (char c : s_octet2) {
        if (!isdigit(c)) {
          all_digits = false;
          break;
        }
      }
      if (all_digits) {
        int octet2 = std::stoi(s_octet2);
        if (octet2 >= 16 && octet2 <= 31) {
          return true;
        }
      }
    }
  }
  return false;
}

static std::string extract_host_from_url(const std::string &url) {
  size_t host_start = url.find("://");
  if (host_start == std::string::npos) {
    host_start = 0;
  } else {
    host_start += 3;
  }
  size_t host_end = url.find_first_of(":/", host_start);
  if (host_end == std::string::npos) {
    return url.substr(host_start);
  }
  return url.substr(host_start, host_end - host_start);
}

static std::string resolve_hostname(const std::string &hostname) {
  struct addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  int err = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
  if (err != 0 || res == nullptr) {
    ESP_LOGE("VigWhip", "DNS lookup failed for %s, err=%d", hostname.c_str(), err);
    return hostname;
  }

  struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
  freeaddrinfo(res);
  return std::string(ip_str);
}

WhipPublisher::WhipPublisher(const std::string &whip_url,
                             const std::string &stream_token,
                             const std::string &dtls_cert_pem,
                             const std::string &dtls_key_pem)
    : whip_url_(whip_url), stream_token_(stream_token), dtls_cert_pem_(dtls_cert_pem),
      dtls_key_pem_(dtls_key_pem) {}

WhipPublisher::~WhipPublisher() { stop(); }

Expected<void> WhipPublisher::start() {
  ESP_LOGI(TAG, "Starting WHIP Publisher session initialization...");

  rx_buffer_.clear();
  rx_offset_ = 0;
  timer_ctx_ = DtlsTimer{};
  received_client_hello_ = false;
  has_error_ = false;

  // 1. Generate local DTLS Certificates and Private Keys using PSA Hardware Engine
  auto cert_status = generate_dtls_cert();
  if (!cert_status) {
    ESP_LOGE(TAG, "DTLS Certificate generation failed");
    return cert_status;
  }

  // 2. Compute the cryptographic fingerprint for our local SDP configuration
  std::string fingerprint = cert_fingerprint_sha256();
  if (fingerprint.empty()) {
    ESP_LOGE(TAG, "Failed to compute local certificate fingerprint");
    return std::unexpected(DeviceError::InternalError);
  }

  // 3. Generate randomized WebRTC ICE local credentials (ufrag and pwd)
  generate_ice_credentials();

  // 4. Build the local WebRTC SDP Offer string
  std::string sdp_offer = build_sdp_offer(fingerprint);
  ESP_LOGD(TAG, "Generated Local SDP Offer:\n%s", sdp_offer.c_str());

  // 5. Post the SDP Offer to the remote WHIP Server to negotiate connection parameters
  auto sdp_answer_or = negotiate_sdp(sdp_offer);
  if (!sdp_answer_or) {
    ESP_LOGE(TAG, "HTTP WHIP SDP signaling negotiation failed");
    return std::unexpected(sdp_answer_or.error());
  }
  std::string sdp_answer = sdp_answer_or.value();
  ESP_LOGI(TAG, "Received Remote SDP Answer:\n%s", sdp_answer.c_str());

  // 6. Parse the Remote Peer's SDP Answer to extract their ICE credentials and media
  // endpoint
  auto parse_status = parse_sdp_answer(sdp_answer);
  if (!parse_status) {
    ESP_LOGE(TAG, "Failed to parse remote SDP answer payload");
    return parse_status;
  }

  // 6b. Now that we know the DTLS role from the SDP answer, configure the SSL session
  auto dtls_session_status = configure_dtls_session();
  if (!dtls_session_status) {
    ESP_LOGE(TAG, "Failed to configure DTLS session");
    return dtls_session_status;
  }

  // 7. Establish the Media Transport Layer (WebRTC media mandates UDP)
  udp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create media UDP socket. errno=%d", errno);
    return std::unexpected(DeviceError::InternalError);
  }

  // Set socket to non-blocking so DTLS handshake timeouts work correctly
  int flags = fcntl(udp_socket_, F_GETFL, 0);
  fcntl(udp_socket_, F_SETFL, flags | O_NONBLOCK);

  // 8. Bind the Mbed TLS SSL Context to our newly generated UDP Socket
  mbedtls_ssl_set_bio(&ssl_ctx_, this, WhipPublisher::dtls_send,
                      WhipPublisher::dtls_recv, WhipPublisher::dtls_recv_timeout);

  ESP_LOGI(TAG, "Media socket initialized. Target Remote Endpoint: %s:%d",
           remote_ip_.c_str(), remote_port_);

  // 9. Execute STUN Binding requests (ICE Connectivity checks)
  auto ice_status = do_ice_binding();
  if (!ice_status) {
    ESP_LOGE(TAG, "ICE connectivity checks failed to establish a route");
    stop();
    return ice_status;
  }
  // 10. Drain any stale STUN responses from the socket before DTLS
  {
    uint8_t drain_buf[2048];
    int drained = 0;
    while (true) {
      int r = recv(udp_socket_, drain_buf, sizeof(drain_buf), MSG_DONTWAIT);
      if (r <= 0)
        break;
      drained++;
      ESP_LOGD(TAG, "Drained stale packet %d: %d bytes, first byte=0x%02X", drained, r,
               drain_buf[0]);
    }
    if (drained > 0) {
      ESP_LOGI(TAG, "Drained %d stale packets from socket before DTLS", drained);
    }
  }

  // 11. Perform the DTLS-SRTP Handshake over the verified UDP route
  ESP_LOGI(TAG, "Initiating DTLS-SRTP handshake...");
  int ret;
  int handshake_attempts = 0;
  while ((ret = mbedtls_ssl_handshake(&ssl_ctx_)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      char err_buf[128];
      mbedtls_strerror(ret, err_buf, sizeof(err_buf));
      ESP_LOGE(TAG, "DTLS handshake failed with error: -0x%04X (%s)", -ret, err_buf);
      stop();
      return std::unexpected(DeviceError::InternalError);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    handshake_attempts++;
    if (handshake_attempts > 500) { // 5 seconds timeout limit
      ESP_LOGE(TAG, "DTLS handshake timed out");
      stop();
      return std::unexpected(DeviceError::InternalError);
    }
  }
  ESP_LOGI(TAG, "DTLS-SRTP handshake completed successfully!");

  // 11. Extract keying material and configure internal SRTP context states
  auto srtp_status = setup_srtp();
  if (!srtp_status) {
    ESP_LOGE(TAG, "Failed to derive keys and configure SRTP context");
    stop();
    return srtp_status;
  }

  ESP_LOGI(TAG, "WHIP Publisher fully initialized and streaming live media.");
  return {};
}

void WhipPublisher::stop() {
  if (srtp_send_.initialized) {
    if (srtp_send_.cipher_key_id != 0) {
      psa_destroy_key(srtp_send_.cipher_key_id);
      srtp_send_.cipher_key_id = 0;
    }
    if (srtp_send_.auth_key_id != 0) {
      psa_destroy_key(srtp_send_.auth_key_id);
      srtp_send_.auth_key_id = 0;
    }
    srtp_send_.initialized = false;
  }
  if (dtls_initialized_) {
    mbedtls_ssl_free(&ssl_ctx_);
    mbedtls_ssl_config_free(&ssl_conf_);
    dtls_initialized_ = false;
  }
  mbedtls_x509_crt_free(&cert_);
  mbedtls_pk_free(&pkey_);
  if (udp_socket_ >= 0) {
    close(udp_socket_);
    udp_socket_ = -1;
  }
}

Expected<std::string> WhipPublisher::negotiate_sdp(const std::string &local_sdp) {
  esp_http_client_config_t config = {};
  config.url = whip_url_.c_str();
  config.method = HTTP_METHOD_POST;
  config.buffer_size = 4096;
  config.buffer_size_tx = 4096;
  config.timeout_ms = 5000;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client)
    return std::unexpected(DeviceError::InternalError);

  std::string auth_header = "Bearer " + stream_token_;
  esp_http_client_set_header(client, "Authorization", auth_header.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/sdp");

  esp_err_t err = esp_http_client_open(client, local_sdp.length());
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  int write_len = esp_http_client_write(client, local_sdp.c_str(), local_sdp.length());
  if (write_len < 0) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  int content_length = esp_http_client_fetch_headers(client);
  if (content_length < 0) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  int status = esp_http_client_get_status_code(client);
  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "WHIP server returned status %d", status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  std::string response;
  char buf[512];
  while (true) {
    int read = esp_http_client_read_response(client, buf, sizeof(buf));
    if (read < 0) {
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return std::unexpected(DeviceError::HttpPayloadError);
    }
    if (read == 0) {
      break;
    }
    response.append(buf, static_cast<size_t>(read));
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return response;
}

Expected<void> WhipPublisher::parse_sdp_answer(const std::string &answer) {
  std::istringstream stream(answer);
  std::string line;

  remote_ip_ = "";
  remote_port_ = 0;
  remote_ufrag_ = "";
  remote_pwd_ = "";
  dtls_role_is_server_ = false;

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.rfind("a=ice-ufrag:", 0) == 0) {
      remote_ufrag_ = line.substr(12);
    } else if (line.rfind("a=ice-pwd:", 0) == 0) {
      remote_pwd_ = line.substr(10);
    } else if (line.rfind("a=candidate:", 0) == 0) {
      // Parse remote UDP candidate line:
      // a=candidate:XXXX 1 udp YYYY <ip> <port> typ host ...
      std::istringstream cand_stream(line);
      std::string temp, proto;
      cand_stream >> temp;  // a=candidate:XXXX
      cand_stream >> temp;  // Component ID (1)
      cand_stream >> proto; // udp / tcp
      if (proto == "udp" || proto == "UDP") {
        cand_stream >> temp; // priority
        std::string ip;
        uint16_t port;
        cand_stream >> ip;
        cand_stream >> port;
        if (remote_ip_.empty() || remote_ip_ == "127.0.0.1" ||
            remote_ip_ == "0.0.0.0") {
          remote_ip_ = ip;
          remote_port_ = port;
        }
      }
    } else if (line.rfind("c=IN IP4 ", 0) == 0 && remote_ip_.empty()) {
      remote_ip_ = line.substr(9);
    } else if (line.rfind("m=video ", 0) == 0 && remote_port_ == 0) {
      // m=video <port> RTP/SAVPF 96
      size_t port_start = 8;
      size_t port_end = line.find(' ', port_start);
      if (port_end != std::string::npos) {
        remote_port_ = std::stoi(line.substr(port_start, port_end - port_start));
      }
    } else if (line.rfind("a=setup:", 0) == 0) {
      std::string setup_role = line.substr(8);
      // If the remote peer is "active", it will initiate DTLS, so we must be the
      // server. If the remote peer is "passive", we must initiate DTLS as the client.
      dtls_role_is_server_ = (setup_role == "active");
      ESP_LOGI(TAG, "Remote SDP setup role: '%s' -> local DTLS role: %s",
               setup_role.c_str(), dtls_role_is_server_ ? "SERVER" : "CLIENT");
    }
  }

  // Fallback to extraction from WHIP URL host if candidate IP was local loopback or not
  // present
  if (remote_ip_.empty() || remote_ip_ == "127.0.0.1" || remote_ip_ == "0.0.0.0") {
    remote_ip_ = extract_host_from_url(whip_url_);
  }

  // Translate private container/NAT candidates to public-facing load balancer/signaling
  // host if necessary
  std::string signaling_host = extract_host_from_url(whip_url_);
  if (is_private_ip(remote_ip_) && !is_private_ip(signaling_host)) {
    ESP_LOGI(TAG,
             "Parsed remote candidate IP '%s' is a private IP. Translating to "
             "signaling host '%s'...",
             remote_ip_.c_str(), signaling_host.c_str());
    std::string resolved = resolve_hostname(signaling_host);
    if (!resolved.empty() && !is_private_ip(resolved)) {
      ESP_LOGI(TAG, "Successfully resolved '%s' to public IP '%s'",
               signaling_host.c_str(), resolved.c_str());
      remote_ip_ = resolved;
    }
  }

  // If remote_ip_ is still a domain name, resolve it to an IP address so inet_pton
  // works
  if (!remote_ip_.empty() && !isdigit(remote_ip_[0]) &&
      remote_ip_.find(':') == std::string::npos) {
    ESP_LOGI(TAG, "Resolving host '%s' to IP address...", remote_ip_.c_str());
    std::string resolved = resolve_hostname(remote_ip_);
    if (!resolved.empty()) {
      remote_ip_ = resolved;
    }
  }

  if (remote_port_ == 0) {
    remote_port_ = 8189; // MediaMTX WebRTC default UDP ICE Port
  }

  ESP_LOGI(TAG, "Remote ICE Info: ufrag='%s', pwd='%s'", remote_ufrag_.c_str(),
           remote_pwd_.c_str());
  ESP_LOGI(TAG, "Remote Target Endpoint: %s:%d", remote_ip_.c_str(), remote_port_);

  if (!remote_ufrag_.empty() && !remote_pwd_.empty() && !remote_ip_.empty()) {
    return {};
  }
  return std::unexpected(DeviceError::HttpPayloadError);
}

void WhipPublisher::push_frame(const vigo::camera::EncodedFrame &frame) {
  if (udp_socket_ < 0 || frame.data.empty() || !srtp_send_.initialized)
    return;

  // Drain any incoming STUN/RTCP packets and check for connection errors
  uint8_t dummy[256];
  while (true) {
    int r = recv(udp_socket_, dummy, sizeof(dummy), MSG_DONTWAIT);
    if (r < 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
        ESP_LOGW(TAG, "recv failed with errno=%d, marking publisher as errored", errno);
        has_error_ = true;
      }
      break;
    }
  }

  if (has_error_) {
    return;
  }

  uint32_t rtp_ts = static_cast<uint32_t>(frame.pts * 90);
  size_t payload_size = frame.data.size();
  const size_t MAX_PAYLOAD = 1400;

  if (payload_size <= MAX_PAYLOAD) {
    send_rtp_packet(frame.data.data(), payload_size, rtp_ts, true);
  } else {
    // Basic FU-A Fragmentation
    const uint8_t *nal_data = frame.data.data();
    uint8_t nalu_header = nal_data[0];
    uint8_t fu_indicator = (nalu_header & 0xE0) | 28; // FU-A type is 28
    uint8_t fu_header = nalu_header & 0x1F;

    nal_data++;
    payload_size--;

    bool first = true;
    uint8_t fua_buf[MAX_PAYLOAD];
    while (payload_size > 0) {
      size_t chunk_size = std::min(payload_size, MAX_PAYLOAD - 2);
      bool last = (chunk_size == payload_size);

      uint8_t fu_header_mod = fu_header;
      if (first) {
        fu_header_mod |= 0x80; // Start bit
        first = false;
      }
      if (last) {
        fu_header_mod |= 0x40; // End bit
      }

      fua_buf[0] = fu_indicator;
      fua_buf[1] = fu_header_mod;
      memcpy(fua_buf + 2, nal_data, chunk_size);

      send_rtp_packet(fua_buf, chunk_size + 2, rtp_ts, last);

      nal_data += chunk_size;
      payload_size -= chunk_size;
    }
  }
}

void WhipPublisher::send_rtp_packet(const uint8_t *payload, size_t size,
                                    uint32_t timestamp, bool marker) {
  if (udp_socket_ < 0 || !srtp_send_.initialized || has_error_)
    return;

  if (ssrc_ == 0) {
    ssrc_ = 0x12345678;
  }

  // Stack buffer for RTP header (12) + payload (max 1400) + SRTP auth tag (10)
  uint8_t packet[12 + 1400 + 10];
  packet[0] = 0x80;
  packet[1] = (marker ? 0x80 : 0x00) | 96; // PT=96 (dynamic H264 payload type)
  packet[2] = (seq_num_ >> 8) & 0xFF;
  packet[3] = seq_num_ & 0xFF;
  packet[4] = (timestamp >> 24) & 0xFF;
  packet[5] = (timestamp >> 16) & 0xFF;
  packet[6] = (timestamp >> 8) & 0xFF;
  packet[7] = timestamp & 0xFF;
  packet[8] = (ssrc_ >> 24) & 0xFF;
  packet[9] = (ssrc_ >> 16) & 0xFF;
  packet[10] = (ssrc_ >> 8) & 0xFF;
  packet[11] = ssrc_ & 0xFF;

  memcpy(packet + 12, payload, size);

  size_t out_len = 0;
  if (srtp_protect(packet, 12 + size, &out_len)) {
    int sent = send(udp_socket_, packet, out_len, 0);
    if (sent < 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
        ESP_LOGW(TAG, "send failed with errno=%d, marking publisher as errored", errno);
        has_error_ = true;
      }
    }
  }
  seq_num_++;
}

Expected<void> WhipPublisher::generate_dtls_cert() {
  int ret;

  if (psa_crypto_init() != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to initialize PSA Crypto hardware engine");
    return std::unexpected(DeviceError::InternalError);
  }

  mbedtls_pk_init(&pkey_);
  mbedtls_x509_crt_init(&cert_);

  std::string cert_pem = unescape_pem(this->dtls_cert_pem_);
  ret = mbedtls_x509_crt_parse(&cert_, (const unsigned char *)cert_pem.c_str(),
                               cert_pem.length() + 1);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to parse cert PEM: -0x%04X", -ret);
    return std::unexpected(DeviceError::InternalError);
  }

  std::string key_pem = unescape_pem(this->dtls_key_pem_);
  ret = mbedtls_pk_parse_key(&pkey_, (const unsigned char *)key_pem.c_str(),
                             key_pem.length() + 1, NULL, 0);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to parse EC private key: -0x%04X", -ret);
    return std::unexpected(DeviceError::InternalError);
  }

  return {};
}

Expected<void> WhipPublisher::configure_dtls_session() {
  int ret;

  mbedtls_ssl_config_init(&ssl_conf_);
  mbedtls_ssl_init(&ssl_ctx_);

  int dtls_endpoint =
      dtls_role_is_server_ ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT;
  ESP_LOGI(TAG, "Configuring DTLS as %s", dtls_role_is_server_ ? "SERVER" : "CLIENT");
  ret = mbedtls_ssl_config_defaults(&ssl_conf_, dtls_endpoint,
                                    MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    return std::unexpected(DeviceError::InternalError);

  // mbedtls_debug_set_threshold(1);
  // mbedtls_ssl_conf_dbg(&ssl_conf_, mbedtls_debug_cb, NULL);

  ret = mbedtls_ssl_conf_own_cert(&ssl_conf_, &cert_, &pkey_);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to assign cert/key to DTLS: -0x%04X", -ret);
    return std::unexpected(DeviceError::InternalError);
  }

  mbedtls_ssl_conf_authmode(&ssl_conf_, MBEDTLS_SSL_VERIFY_NONE);

  // When acting as DTLS server, disable HelloVerifyRequest cookie.
  // ICE has already verified the peer's address, and WebRTC clients
  // don't expect a HelloVerifyRequest round-trip.
  if (dtls_role_is_server_) {
    mbedtls_ssl_conf_dtls_cookies(&ssl_conf_, NULL, NULL, NULL);
  }

  // Cap DTLS retransmission timeouts: min 2s, max 4s (default is 1s/60s which totals
  // ~280s)
  mbedtls_ssl_conf_handshake_timeout(&ssl_conf_, 2000, 4000);

  static mbedtls_ssl_srtp_profile srtp_profiles[] = {
      MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80, MBEDTLS_TLS_SRTP_UNSET};

  ret = mbedtls_ssl_conf_dtls_srtp_protection_profiles(&ssl_conf_, srtp_profiles);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to configure DTLS-SRTP profiles: -0x%04X", -ret);
    return std::unexpected(DeviceError::InternalError);
  }

  ret = mbedtls_ssl_setup(&ssl_ctx_, &ssl_conf_);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to setup SSL context: -0x%04X", -ret);
    return std::unexpected(DeviceError::InternalError);
  }

  // DTLS mandates a retransmission timer for handshake flight retransmits over UDP
  mbedtls_ssl_set_timer_cb(&ssl_ctx_, &timer_ctx_, WhipPublisher::dtls_timing_set_delay,
                           WhipPublisher::dtls_timing_get_delay);

  dtls_keys_.keys_exported = false;
  mbedtls_ssl_set_export_keys_cb(&ssl_ctx_, srtp_export_keys_cb, &dtls_keys_);

  dtls_initialized_ = true;
  return {};
}

void WhipPublisher::generate_ice_credentials() {
  const char charset[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  local_ufrag_ = "";
  for (int i = 0; i < 8; ++i) {
    local_ufrag_ += charset[esp_random() % (sizeof(charset) - 1)];
  }
  local_pwd_ = "";
  for (int i = 0; i < 24; ++i) {
    local_pwd_ += charset[esp_random() % (sizeof(charset) - 1)];
  }
}

std::string WhipPublisher::cert_fingerprint_sha256() {
  if (cert_.raw.p == nullptr || cert_.raw.len == 0)
    return "";

  uint8_t hash[32];
  int ret = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), cert_.raw.p,
                       cert_.raw.len, hash);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to calculate cert SHA-256 hash: -0x%04X", -ret);
    return "";
  }

  char hex[96];
  char *ptr = hex;
  for (int i = 0; i < 32; ++i) {
    ptr += sprintf(ptr, "%02X%c", hash[i], (i == 31) ? '\0' : ':');
  }
  return std::string(hex);
}

std::string WhipPublisher::build_sdp_offer(const std::string &fingerprint) {
  std::stringstream sdp;
  sdp << "v=0\r\n"
      << "o=- 0 0 IN IP4 127.0.0.1\r\n"
      << "s=VigStream\r\n"
      << "c=IN IP4 127.0.0.1\r\n"
      << "t=0 0\r\n"
      << "a=group:BUNDLE 0\r\n"
      << "m=video 9 RTP/SAVPF 96\r\n"
      << "a=rtpmap:96 H264/90000\r\n"
      << "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
      << "a=setup:actpass\r\n"
      << "a=mid:0\r\n"
      << "a=sendonly\r\n"
      << "a=rtcp-mux\r\n"
      << "a=ice-ufrag:" << local_ufrag_ << "\r\n"
      << "a=ice-pwd:" << local_pwd_ << "\r\n"
      << "a=fingerprint:sha-256 " << fingerprint << "\r\n";
  return sdp.str();
}

Expected<void> WhipPublisher::do_ice_binding() {
  ESP_LOGI(TAG, "Starting ICE Connectivity checks (STUN binding request)...");

  std::vector<uint8_t> stun;

  // Header: Type=0x0001 (Binding Request), Length=0 (placeholder), Magic=0x2112A442
  stun.push_back(0x00);
  stun.push_back(0x01);
  stun.push_back(0x00);
  stun.push_back(0x00);
  stun.push_back(0x21);
  stun.push_back(0x12);
  stun.push_back(0xA4);
  stun.push_back(0x42);

  uint8_t tid[12];
  for (int i = 0; i < 12; ++i) {
    tid[i] = esp_random() & 0xFF;
    stun.push_back(tid[i]);
  }

  std::string username = remote_ufrag_ + ":" + local_ufrag_;
  append_stun_attr(stun, 0x0006, (const uint8_t *)username.data(), username.length());

  uint32_t prio = htonl(1853824767);
  append_stun_attr(stun, 0x0024, (const uint8_t *)&prio, 4);

  uint64_t controlling = 0x123456789ABCDEF0ULL;
  append_stun_attr(stun, 0x802A, (const uint8_t *)&controlling, 8);

  append_stun_attr(stun, 0x0025, nullptr, 0);

  uint16_t stun_len = stun.size() - 20;
  uint16_t total_attr_len =
      stun_len + 24; // Message-Integrity takes 24 bytes (4 header + 20 SHA1)
  stun[2] = (total_attr_len >> 8) & 0xFF;
  stun[3] = total_attr_len & 0xFF;

  uint8_t hmac[20];
  size_t hmac_len = 0;
  psa_hmac_sha1((const uint8_t *)remote_pwd_.data(), remote_pwd_.length(), stun.data(),
                stun.size(), hmac, sizeof(hmac), &hmac_len);

  append_stun_attr(stun, 0x0008, hmac, 20);

  total_attr_len += 8; // Add Fingerprint attribute size
  stun[2] = (total_attr_len >> 8) & 0xFF;
  stun[3] = total_attr_len & 0xFF;

  uint32_t fp = crc32(stun.data(), stun.size()) ^ 0x5354554E;
  fp = htonl(fp);
  append_stun_attr(stun, 0x8028, (const uint8_t *)&fp, 4);

  struct sockaddr_in servaddr = {};
  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(remote_port_);
  inet_pton(AF_INET, remote_ip_.c_str(), &servaddr.sin_addr);

  if (connect(udp_socket_, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
    ESP_LOGE(TAG, "Failed to connect UDP socket to remote endpoint");
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  for (int retry = 0; retry < 5; ++retry) {
    send(udp_socket_, stun.data(), stun.size(), 0);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  uint8_t recv_buf[512];
  int received = 0;
  for (int wait = 0; wait < 20; ++wait) {
    received = recv(udp_socket_, recv_buf, sizeof(recv_buf), 0);
    if (received >= 20 && recv_buf[0] == 0x01 && recv_buf[1] == 0x01) {
      ESP_LOGI(TAG, "ICE connectivity verified (received STUN success response)");
      return {};
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGW(
      TAG,
      "ICE STUN response not received, proceeding to DTLS handshake regardless...");
  return {};
}

int WhipPublisher::dtls_send(void *ctx, const unsigned char *buf, size_t len) {
  WhipPublisher *self = static_cast<WhipPublisher *>(ctx);
  ESP_LOGI("VigWhip", "dtls_send: sending %zu bytes (content_type=0x%02X)", len,
           len > 0 ? buf[0] : 0);
  int sent = send(self->udp_socket_, buf, len, 0);
  if (sent < 0) {
    ESP_LOGE("VigWhip", "dtls_send: send() failed, errno=%d", errno);
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  ESP_LOGI("VigWhip", "dtls_send: sent %d bytes successfully", sent);
  return sent;
}

int WhipPublisher::dtls_recv(void *ctx, unsigned char *buf, size_t len) {
  WhipPublisher *self = static_cast<WhipPublisher *>(ctx);

  if (self->rx_buffer_.empty() || self->rx_offset_ >= self->rx_buffer_.size()) {
    self->rx_buffer_.clear();
    self->rx_offset_ = 0;

    uint8_t staging[2048];
    while (true) {
      int r = recv(self->udp_socket_, staging, sizeof(staging), 0);
      if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return MBEDTLS_ERR_SSL_WANT_READ;
        }
        ESP_LOGE("VigWhip", "dtls_recv socket error: errno=%d", errno);
        return MBEDTLS_ERR_NET_RECV_FAILED;
      }
      if (r == 0) {
        return 0;
      }

      // Check if it is a STUN packet (first byte 0x00 or 0x01)
      if (staging[0] == 0x00 || staging[0] == 0x01) {
        ESP_LOGI("VigWhip", "dtls_recv: Got STUN packet of %d bytes", r);
        if (r >= 20 && staging[0] == 0x00 && staging[1] == 0x01) {
          ESP_LOGI("VigWhip", "Processing incoming STUN binding request...");
          self->send_stun_binding_response(staging + 8);
        }
        continue;
      }

      // If it is a DTLS packet, first byte must be in [20, 63]
      if (staging[0] >= 20 && staging[0] <= 63) {
        ESP_LOGI("VigWhip",
                 "dtls_recv: got %d DTLS bytes, first_byte=0x%02X, req_len=%zu", r,
                 staging[0], len);
        self->received_client_hello_ = true;
        self->rx_buffer_.assign(staging, staging + r);
        self->rx_offset_ = 0;
        break;
      }

      ESP_LOGW("VigWhip",
               "dtls_recv: ignoring non-DTLS packet of %d bytes, first_byte=0x%02X", r,
               staging[0]);
    }
  }

  size_t available = self->rx_buffer_.size() - self->rx_offset_;
  size_t to_copy = std::min(len, available);
  std::memcpy(buf, self->rx_buffer_.data() + self->rx_offset_, to_copy);
  self->rx_offset_ += to_copy;

  if (self->rx_offset_ >= self->rx_buffer_.size()) {
    self->rx_buffer_.clear();
    self->rx_offset_ = 0;
  }

  return to_copy;
}

void WhipPublisher::send_stun_binding_response(const uint8_t *tid) {
  std::vector<uint8_t> stun;

  // 1. Header: Type=0x0101 (Binding Success Response), Length=0 (placeholder),
  // Magic=0x2112A442
  stun.push_back(0x01);
  stun.push_back(0x01);
  stun.push_back(0x00);
  stun.push_back(0x00);
  stun.push_back(0x21);
  stun.push_back(0x12);
  stun.push_back(0xA4);
  stun.push_back(0x42);

  // 2. Transaction ID (12 bytes)
  for (int i = 0; i < 12; ++i) {
    stun.push_back(tid[i]);
  }

  // 3. XOR-MAPPED-ADDRESS attribute (type 0x0020)
  uint8_t xor_mapped[8];
  xor_mapped[0] = 0x00; // Reserved
  xor_mapped[1] = 0x01; // IPv4 Family

  xor_mapped[2] = (remote_port_ >> 8) ^ 0x21;
  xor_mapped[3] = (remote_port_ & 0xFF) ^ 0x12;

  uint32_t ip_val = 0;
  inet_pton(AF_INET, remote_ip_.c_str(), &ip_val);
  uint8_t *ip_bytes = reinterpret_cast<uint8_t *>(&ip_val);
  xor_mapped[4] = ip_bytes[0] ^ 0x21;
  xor_mapped[5] = ip_bytes[1] ^ 0x12;
  xor_mapped[6] = ip_bytes[2] ^ 0xA4;
  xor_mapped[7] = ip_bytes[3] ^ 0x42;

  append_stun_attr(stun, 0x0020, xor_mapped, 8);

  // 4. MESSAGE-INTEGRITY attribute (type 0x0008)
  uint16_t stun_len = stun.size() - 20;
  uint16_t total_attr_len =
      stun_len + 24; // Message-Integrity takes 24 bytes (4 header + 20 SHA1)
  stun[2] = (total_attr_len >> 8) & 0xFF;
  stun[3] = total_attr_len & 0xFF;

  uint8_t hmac[20];
  size_t hmac_len = 0;
  psa_hmac_sha1((const uint8_t *)local_pwd_.data(), local_pwd_.length(), stun.data(),
                stun.size(), hmac, sizeof(hmac), &hmac_len);

  append_stun_attr(stun, 0x0008, hmac, 20);

  // 5. FINGERPRINT attribute (type 0x8028)
  total_attr_len += 8; // Fingerprint is 8 bytes (4 header + 4 CRC32)
  stun[2] = (total_attr_len >> 8) & 0xFF;
  stun[3] = total_attr_len & 0xFF;

  uint32_t fp = crc32(stun.data(), stun.size()) ^ 0x5354554E;
  fp = htonl(fp);
  append_stun_attr(stun, 0x8028, (const uint8_t *)&fp, 4);

  ESP_LOGI("VigWhip", "Sending STUN success response to %s:%d", remote_ip_.c_str(),
           remote_port_);
  send(udp_socket_, stun.data(), stun.size(), 0);
}

int WhipPublisher::dtls_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                     uint32_t timeout) {
  WhipPublisher *self = static_cast<WhipPublisher *>(ctx);

  if (!self->rx_buffer_.empty()) {
    return dtls_recv(ctx, buf, len);
  }

  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(self->udp_socket_, &read_fds);

  struct timeval tv;
  tv.tv_sec = timeout / 1000;
  tv.tv_usec = (timeout % 1000) * 1000;

  int ret = select(self->udp_socket_ + 1, &read_fds, NULL, NULL, &tv);
  if (ret <= 0) {
    if (!self->received_client_hello_) {
      return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_SSL_TIMEOUT;
  }

  return dtls_recv(ctx, buf, len);
}

void WhipPublisher::dtls_timing_set_delay(void *data, uint32_t int_ms,
                                          uint32_t fin_ms) {
  DtlsTimer *timer = static_cast<DtlsTimer *>(data);
  timer->int_ms = int_ms;
  timer->fin_ms = fin_ms;
  if (fin_ms == 0) {
    timer->start_time_us = 0;
    return;
  }
  timer->start_time_us = esp_timer_get_time();
}

int WhipPublisher::dtls_timing_get_delay(void *data) {
  DtlsTimer *timer = static_cast<DtlsTimer *>(data);
  if (timer->fin_ms == 0) {
    return -1;
  }
  int64_t elapsed_ms = (esp_timer_get_time() - timer->start_time_us) / 1000;
  if (elapsed_ms >= timer->fin_ms) {
    return 2;
  }
  if (elapsed_ms >= timer->int_ms) {
    return 1;
  }
  return 0;
}

void WhipPublisher::srtp_export_keys_cb(void *p_expkey,
                                        mbedtls_ssl_key_export_type type,
                                        const unsigned char *secret, size_t secret_len,
                                        const unsigned char client_random[32],
                                        const unsigned char server_random[32],
                                        mbedtls_tls_prf_types tls_prf_type) {
  if (type == MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET && secret_len == 48) {
    auto *keys = static_cast<DtlsSrtpKeys *>(p_expkey);
    std::memcpy(keys->master_secret, secret, 48);
    std::memcpy(keys->client_random, client_random, 32);
    std::memcpy(keys->server_random, server_random, 32);
    keys->tls_prf_type = tls_prf_type;
    keys->keys_exported = true;
  }
}

Expected<void> WhipPublisher::setup_srtp() {
  if (!dtls_keys_.keys_exported) {
    ESP_LOGE(TAG, "DTLS handshake completed but master secret not exported!");
    return std::unexpected(DeviceError::InternalError);
  }

  uint8_t keying_material[60];
  uint8_t seed[64];
  std::memcpy(seed, dtls_keys_.client_random, 32);
  std::memcpy(seed + 32, dtls_keys_.server_random, 32);

  int ret = mbedtls_ssl_tls_prf(dtls_keys_.tls_prf_type, dtls_keys_.master_secret, 48,
                                "EXTRACTOR-dtls_srtp", seed, 64, keying_material,
                                sizeof(keying_material));
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to run TLS PRF to derive DTLS-SRTP keying material: -0x%04X",
             -ret);
    return std::unexpected(DeviceError::InternalError);
  }

  // RFC 5764 Section 4.2 keying material layout:
  //   client_write_SRTP_master_key  [16 bytes] offset 0
  //   server_write_SRTP_master_key  [16 bytes] offset 16
  //   client_write_SRTP_master_salt [14 bytes] offset 32
  //   server_write_SRTP_master_salt [14 bytes] offset 46
  // Use server_write keys when we are the DTLS server, client_write otherwise.
  uint8_t master_key[16];
  uint8_t master_salt[14];
  if (dtls_role_is_server_) {
    std::memcpy(master_key, keying_material + 16, 16);
    std::memcpy(master_salt, keying_material + 46, 14);
  } else {
    std::memcpy(master_key, keying_material, 16);
    std::memcpy(master_salt, keying_material + 32, 14);
  }

  srtp_kdf(master_key, master_salt, 0x00, srtp_send_.key, 16);
  srtp_kdf(master_key, master_salt, 0x02, srtp_send_.salt, 14);
  srtp_kdf(master_key, master_salt, 0x01, srtp_send_.auth_key, 20);

  // Pre-import the AES cipher key into PSA once (avoid per-packet import/destroy)
  {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CTR);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_status_t st =
        psa_import_key(&attr, srtp_send_.key, 16, &srtp_send_.cipher_key_id);
    if (st != PSA_SUCCESS) {
      ESP_LOGE(TAG, "Failed to pre-import SRTP cipher key: %d", (int)st);
      return std::unexpected(DeviceError::InternalError);
    }
  }

  // Pre-import the HMAC auth key into PSA once
  {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_1));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_status_t st =
        psa_import_key(&attr, srtp_send_.auth_key, 20, &srtp_send_.auth_key_id);
    if (st != PSA_SUCCESS) {
      ESP_LOGE(TAG, "Failed to pre-import SRTP auth key: %d", (int)st);
      return std::unexpected(DeviceError::InternalError);
    }
  }

  srtp_send_.roc = 0;
  srtp_send_.last_seq = 0;
  srtp_send_.initialized = true;

  ESP_LOGI(TAG, "SRTP Session Keys derived successfully!");
  return {};
}

bool WhipPublisher::srtp_protect(uint8_t *packet, size_t rtp_len, size_t *out_len) {
  if (!srtp_send_.initialized)
    return false;

  uint16_t seq = (packet[2] << 8) | packet[3];

  if (srtp_send_.last_seq > 32768 && seq < 16384) {
    srtp_send_.roc++;
  }
  srtp_send_.last_seq = seq;
  uint64_t index = ((uint64_t)srtp_send_.roc << 16) | seq;

  uint8_t iv[16] = {0};
  memcpy(iv, srtp_send_.salt, 14);
  iv[4] ^= packet[8];
  iv[5] ^= packet[9];
  iv[6] ^= packet[10];
  iv[7] ^= packet[11];
  for (int i = 0; i < 6; ++i) {
    iv[8 + i] ^= (index >> ((5 - i) * 8)) & 0xFF;
  }

  size_t payload_len = rtp_len - 12;
  uint8_t *payload = packet + 12;

  psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
  psa_status_t status =
      psa_cipher_encrypt_setup(&operation, srtp_send_.cipher_key_id, PSA_ALG_CTR);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to setup PSA cipher: %d", (int)status);
    return false;
  }

  status = psa_cipher_set_iv(&operation, iv, 16);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to set IV: %d", (int)status);
    psa_cipher_abort(&operation);
    return false;
  }

  size_t out_len_psa = 0;
  status = psa_cipher_update(&operation, payload, payload_len, payload, payload_len,
                             &out_len_psa);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to update cipher: %d", (int)status);
    psa_cipher_abort(&operation);
    return false;
  }

  size_t finish_len = 0;
  status = psa_cipher_finish(&operation, payload + out_len_psa,
                             payload_len - out_len_psa, &finish_len);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "Failed to finish cipher: %d", (int)status);
    psa_cipher_abort(&operation);
    return false;
  }

  psa_cipher_abort(&operation);

  // HMAC-SHA1-80 authentication using pre-imported key
  // Max rtp_len is 12 + 1400 = 1412, so 1416 bytes suffices
  uint8_t auth_input[1416];
  memcpy(auth_input, packet, rtp_len);
  auth_input[rtp_len] = (srtp_send_.roc >> 24) & 0xFF;
  auth_input[rtp_len + 1] = (srtp_send_.roc >> 16) & 0xFF;
  auth_input[rtp_len + 2] = (srtp_send_.roc >> 8) & 0xFF;
  auth_input[rtp_len + 3] = srtp_send_.roc & 0xFF;

  uint8_t hmac[20];
  size_t hmac_len = 0;
  status = psa_mac_compute(srtp_send_.auth_key_id, PSA_ALG_HMAC(PSA_ALG_SHA_1),
                           auth_input, rtp_len + 4, hmac, sizeof(hmac), &hmac_len);
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "SRTP HMAC compute failed: %d", (int)status);
    return false;
  }

  memcpy(packet + rtp_len, hmac, 10);
  *out_len = rtp_len + 10;
  return true;
}

} // namespace vigo::whip
