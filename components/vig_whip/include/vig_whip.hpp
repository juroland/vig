#pragma once
#include "camera.hpp"
#include "error_types.hpp"
#include <cstdint>
#include <string>
#include <vector>

// mbedTLS DTLS-SRTP
#include "mbedtls/md.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "psa/crypto.h"

namespace vig::camera {
struct EncodedFrame;
}

namespace vig::whip {

// SRTP state — AES-128-CM + HMAC-SHA1-80
struct SrtpContext {
  uint8_t key[16];
  uint8_t salt[14];
  uint8_t auth_key[20];
  // Rolling SRTP index for ROC computation
  uint32_t roc{0};
  uint16_t last_seq{0};
  bool initialized{false};
};

class WhipPublisher {
public:
  WhipPublisher(const std::string &whip_url, const std::string &stream_token);
  ~WhipPublisher();

  /// Perform WHIP signaling (HTTP POST SDP offer), ICE, DTLS handshake, SRTP setup.
  Expected<void> start();

  /// Tear down the UDP socket and DTLS context.
  void stop();

  /// Encrypt frame as SRTP and transmit via UDP to the remote ICE candidate.
  void push_frame(const vig::camera::EncodedFrame &frame);

private:
  std::string whip_url_;
  std::string stream_token_;

  // ICE credentials (local and remote)
  std::string local_ufrag_;
  std::string local_pwd_;
  std::string remote_ufrag_;
  std::string remote_pwd_;

  // Remote media endpoint (from ICE SDP)
  std::string remote_ip_;
  uint16_t remote_port_{0};

  // UDP socket for media
  int udp_socket_{-1};

  // DTLS context
  mbedtls_ssl_context ssl_ctx_;
  mbedtls_ssl_config ssl_conf_;
  mbedtls_x509_crt cert_;
  mbedtls_pk_context pkey_;
  bool dtls_initialized_{false};

  // SRTP transmit context
  SrtpContext srtp_send_;

  // DTLS-SRTP Key material
  struct DtlsSrtpKeys {
    uint8_t master_secret[48];
    uint8_t client_random[32];
    uint8_t server_random[32];
    mbedtls_tls_prf_types tls_prf_type;
    bool keys_exported{false};
  } dtls_keys_;

  // RTP state
  uint16_t seq_num_{0};
  uint32_t ssrc_{0};

  // ── Internal helpers ──

  /// Generate random ICE ufrag (4 chars) and password (22 chars)
  void generate_ice_credentials();

  /// Generate self-signed ECDSA certificate + private key for DTLS
  Expected<void> generate_dtls_cert();

  /// Compute the SHA-256 fingerprint of our certificate (for SDP)
  std::string cert_fingerprint_sha256();

  /// Build SDP offer string
  std::string build_sdp_offer(const std::string &fingerprint);

  Expected<std::string> negotiate_sdp(const std::string &sdp_offer);

  /// Parse SDP answer: extract remote ICE ufrag/pwd and first UDP candidate
  Expected<void> parse_sdp_answer(const std::string &answer);

  /// Perform ICE binding (STUN binding request → response)
  Expected<void> do_ice_binding();

  /// mbedtls UDP send/recv callbacks
  static int dtls_send(void *ctx, const unsigned char *buf, size_t len);
  static int dtls_recv(void *ctx, unsigned char *buf, size_t len);
  static int dtls_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                               uint32_t timeout);
  static void srtp_export_keys_cb(void *p_expkey, mbedtls_ssl_key_export_type type,
                                  const unsigned char *secret, size_t secret_len,
                                  const unsigned char client_random[32],
                                  const unsigned char server_random[32],
                                  mbedtls_tls_prf_types tls_prf_type);

  /// Derive SRTP keys from DTLS keying material and initialise SrtpContext
  Expected<void> setup_srtp();

  /// SRTP-encrypt one RTP packet in-place (AES-128-CM + HMAC-SHA1-80)
  bool srtp_protect(uint8_t *packet, size_t rtp_len, size_t *out_len);

  /// Build and transmit one RTP/SRTP packet
  void send_rtp_packet(const uint8_t *payload, size_t size, uint32_t timestamp,
                       bool marker);
};

} // namespace vig::whip
