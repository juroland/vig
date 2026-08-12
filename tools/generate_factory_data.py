#!/usr/bin/env python3
import sys
import os
import subprocess
import argparse
import tempfile


def generate_dtls_credentials(tmpdir):
    """
    Generates a new secp256r1 EC private key in DER format and a self-signed X.509 certificate in PEM format.
    """
    key_pem_path = os.path.join(tmpdir, "dtls_key.pem")
    key_der_path = os.path.join(tmpdir, "dtls_key.der")
    cert_pem_path = os.path.join(tmpdir, "dtls_cert.pem")

    # Generate EC Private Key (secp256r1 / prime256v1)
    ec_cmd = [
        "openssl",
        "ecparam",
        "-name",
        "prime256v1",
        "-genkey",
        "-noout",
        "-out",
        key_pem_path,
    ]
    subprocess.run(ec_cmd, check=True, capture_output=True)

    # Convert EC Private Key to DER (binary format)
    der_cmd = [
        "openssl",
        "ec",
        "-in",
        key_pem_path,
        "-outform",
        "DER",
        "-out",
        key_der_path,
    ]
    subprocess.run(der_cmd, check=True, capture_output=True)

    # Generate Self-signed Certificate
    req_cmd = [
        "openssl",
        "req",
        "-new",
        "-x509",
        "-key",
        key_pem_path,
        "-out",
        cert_pem_path,
        "-days",
        "3650",
        "-subj",
        "/CN=VigoDevice",
    ]
    subprocess.run(req_cmd, check=True, capture_output=True)

    with open(key_der_path, "rb") as f:
        key_der = f.read()

    with open(cert_pem_path, "r") as f:
        cert_pem = f.read()

    return key_der, cert_pem


def parse_defaults_file(filepath):
    """
    Parses key-value configuration options from a device defaults configuration file.
    """
    config = {}
    if not os.path.exists(filepath):
        print(f"Error: Defaults file '{filepath}' not found.", file=sys.stderr)
        sys.exit(1)
    with open(filepath, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line:
                key, val = line.split("=", 1)
                key = key.strip()
                val = val.strip()
                # Strip surrounding quotes if any
                if val.startswith('"') and val.endswith('"'):
                    val = val[1:-1]
                config[key] = val
    return config


def unescape_pem(val):
    """
    Replaces literal backslash n sequences with actual newlines.
    """
    return val.replace("\\\\n", "\n").replace("\\n", "\n")


def pem_key_to_der(pem_key_str, tmpdir):
    """
    Converts a PEM format private key into DER binary format.
    """
    pem_path = os.path.join(tmpdir, "key.pem")
    der_path = os.path.join(tmpdir, "key.der")
    with open(pem_path, "w") as f:
        f.write(pem_key_str)

    cmd = ["openssl", "ec", "-in", pem_path, "-outform", "DER", "-out", der_path]
    subprocess.run(cmd, check=True, capture_output=True)
    with open(der_path, "rb") as f:
        return f.read()


def main():
    parser = argparse.ArgumentParser(
        description="Generate Vigo Factory Provisioning NVS Blob"
    )
    parser.add_argument("--bin-out", required=True, help="Output binary file path")
    parser.add_argument("--csv-out", help="Optional output CSV file path")
    parser.add_argument("--hardware-id", help="Unique hardware identifier")
    parser.add_argument("--device-token", help="Device token")
    parser.add_argument(
        "--defaults-file",
        help="Path to device defaults file (e.g. configs/jr.defaults)",
    )
    parser.add_argument(
        "--size", default="0x4000", help="Size of the fct_nvs partition in hex or dec"
    )
    parser.add_argument("--idf-path", help="Path to ESP-IDF installation")
    parser.add_argument(
        "--network-type",
        choices=["wifi", "ethernet"],
        help="Network type (wifi or ethernet)",
    )
    parser.add_argument("--wifi-ssid", help="WiFi SSID")
    parser.add_argument("--wifi-password", help="WiFi password")

    args = parser.parse_args()

    # Determine size
    size_str = args.size
    if size_str.startswith("0x") or size_str.startswith("0X"):
        size = int(size_str, 16)
    else:
        size = int(size_str)

    idf_path = args.idf_path or os.environ.get("IDF_PATH")
    if not idf_path:
        # Check standard location
        standard_path = os.path.expanduser("~/.espressif/v6.0.1/esp-idf")
        if os.path.isdir(standard_path):
            idf_path = standard_path
        else:
            print(
                "Error: IDF_PATH is not set and standard installation not found.",
                file=sys.stderr,
            )
            sys.exit(1)

    gen_script = os.path.join(
        idf_path,
        "components",
        "nvs_flash",
        "nvs_partition_generator",
        "nvs_partition_gen.py",
    )
    if not os.path.isfile(gen_script):
        print(f"Error: nvs_partition_gen.py not found at {gen_script}", file=sys.stderr)
        sys.exit(1)

    # Resolve parameters
    hardware_id = args.hardware_id
    device_token = args.device_token
    cert_pem = None
    key_der = None
    network_type = args.network_type
    wifi_ssid = args.wifi_ssid
    wifi_password = args.wifi_password

    if args.defaults_file:
        print(f"Loading factory parameters from defaults file: {args.defaults_file}")
        config = parse_defaults_file(args.defaults_file)

        if not hardware_id:
            hardware_id = config.get("CONFIG_VIGO_HARDWARE_ID")
        if not device_token:
            device_token = config.get("CONFIG_VIGO_DEVICE_TOKEN")

        if not network_type:
            network_type = config.get("CONFIG_VIGO_NETWORK_TYPE") or "ethernet"

        if not wifi_ssid:
            wifi_ssid = config.get("CONFIG_VIGO_WIFI_SSID") or ""
        if not wifi_password:
            wifi_password = config.get("CONFIG_VIGO_WIFI_PASSWORD") or ""

        cert_pem_raw = config.get("CONFIG_VIGO_DTLS_CERT_PEM")
        key_pem_raw = config.get("CONFIG_VIGO_DTLS_KEY_PEM")

        if not hardware_id or not device_token:
            print(
                "Error: CONFIG_VIGO_HARDWARE_ID or CONFIG_VIGO_DEVICE_TOKEN not found in defaults file.",
                file=sys.stderr,
            )
            sys.exit(1)

        if cert_pem_raw and key_pem_raw:
            cert_pem = unescape_pem(cert_pem_raw)
            key_pem = unescape_pem(key_pem_raw)
            with tempfile.TemporaryDirectory() as tmpdir:
                try:
                    key_der = pem_key_to_der(key_pem, tmpdir)
                except Exception as e:
                    print(
                        f"Error converting PEM key from defaults file to DER format: {e}",
                        file=sys.stderr,
                    )
                    sys.exit(1)
        else:
            print(
                "Warning: CONFIG_VIGO_DTLS_CERT_PEM or CONFIG_VIGO_DTLS_KEY_PEM not found in defaults file.",
                file=sys.stderr,
            )

    # Fallbacks for command-line arguments and openSSL generation
    if not hardware_id:
        hardware_id = "VIGO-DEV-001"
    if not device_token:
        device_token = "setup_token_value_placeholder"

    with tempfile.TemporaryDirectory() as tmpdir:
        if cert_pem is None or key_der is None:
            print(
                "Generating new DTLS credentials (no valid key/cert found in defaults file)..."
            )
            key_der, cert_pem = generate_dtls_credentials(tmpdir)

        # Write them to files inside tmpdir so they can be referenced by the CSV generator
        key_file_path = os.path.join(tmpdir, "dtls_key.der")
        cert_file_path = os.path.join(tmpdir, "dtls_cert.pem")

        with open(key_file_path, "wb") as f:
            f.write(key_der)

        with open(cert_file_path, "w") as f:
            f.write(cert_pem)

        # Generate the CSV content
        csv_lines = [
            "key,type,encoding,value",
            "factory,namespace,,",
            f"hardware_id,data,string,{hardware_id}",
            f"device_token,data,string,{device_token}",
            f"dtls_cert,file,string,{cert_file_path}",
            f"dtls_key,file,binary,{key_file_path}",
            f"network_type,data,string,{network_type}",
            f"wifi_ssid,data,string,{wifi_ssid}",
            f"wifi_password,data,string,{wifi_password}",
        ]
        csv_content = "\n".join(csv_lines) + "\n"

        csv_tmp_path = os.path.join(tmpdir, "factory.csv")
        with open(csv_tmp_path, "w") as f:
            f.write(csv_content)

        if args.csv_out:
            doc_csv_lines = [
                "key,type,encoding,value",
                "factory,namespace,,",
                f"hardware_id,data,string,{hardware_id}",
                f"device_token,data,string,{device_token}",
                "dtls_cert,file,string,configs/dtls_cert.pem",
                "dtls_key,file,binary,configs/dtls_key.der",
            ]
            with open(args.csv_out, "w") as f:
                f.write("\n".join(doc_csv_lines) + "\n")
            print(f"Template CSV written to: {args.csv_out}")

        # Run the nvs_partition_gen.py utility
        cmd = [
            sys.executable,
            gen_script,
            "generate",
            csv_tmp_path,
            args.bin_out,
            str(size),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Error generating partition binary: {res.stderr}", file=sys.stderr)
            sys.exit(1)

        print(
            f"Successfully generated factory partition binary: {args.bin_out} (size: {size} bytes)"
        )


if __name__ == "__main__":
    main()
