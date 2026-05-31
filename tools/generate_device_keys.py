#!/usr/bin/env python3
import sys
import os
import subprocess
import tempfile

def generate_keys():
    """
    Generates a new secp256r1 EC private key and a self-signed X.509 certificate
    using the openssl CLI. Returns a tuple of (private_key_pem, cert_pem).
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        key_path = os.path.join(tmpdir, "device.key")
        cert_path = os.path.join(tmpdir, "device.crt")

        # Generate EC Private Key (secp256r1 / prime256v1)
        ec_cmd = [
            "openssl", "ecparam",
            "-name", "prime256v1",
            "-genkey",
            "-noout",
            "-out", key_path
        ]
        subprocess.run(ec_cmd, check=True, capture_output=True)

        # Generate Self-signed Certificate
        req_cmd = [
            "openssl", "req",
            "-new",
            "-x509",
            "-key", key_path,
            "-out", cert_path,
            "-days", "3650",
            "-subj", "/CN=VigoDevice"
        ]
        subprocess.run(req_cmd, check=True, capture_output=True)

        with open(key_path, "r") as f:
            key_pem = f.read()

        with open(cert_path, "r") as f:
            cert_pem = f.read()

        return key_pem, cert_pem

def format_pem_for_sdkconfig(pem_content: str) -> str:
    """
    Formats PEM content for use in a .defaults / sdkconfig file:
    replaces actual newlines with literal '\\n' sequences.
    """
    # Replace normal newlines with double-escaped "\\n" strings so that Kconfig
    # parser correctly preserves the backslash instead of stripping it to "n".
    escaped = pem_content.replace("\n", "\\\\n")
    return f'"{escaped}"'

def main():
    if len(sys.argv) > 2:
        print("Usage: python3 generate_device_keys.py [path_to_defaults_file]", file=sys.stderr)
        sys.exit(1)

    target_file = sys.argv[1] if len(sys.argv) == 2 else None

    try:
        key_pem, cert_pem = generate_keys()
    except subprocess.CalledProcessError as e:
        print(f"Error executing openssl: {e.stderr.decode()}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print("Error: openssl command not found. Please install openssl first.", file=sys.stderr)
        sys.exit(1)

    cert_config_val = format_pem_for_sdkconfig(cert_pem)
    key_config_val = format_pem_for_sdkconfig(key_pem)

    lines_to_add = [
        f"CONFIG_VIGO_DTLS_CERT_PEM={cert_config_val}\n",
        f"CONFIG_VIGO_DTLS_KEY_PEM={key_config_val}\n"
    ]

    if target_file:
        # Check if target file exists
        if not os.path.exists(target_file):
            print(f"Error: Target file '{target_file}' does not exist.", file=sys.stderr)
            sys.exit(1)

        # Read target file to see if CONFIG_VIGO_DTLS_CERT_PEM/KEY_PEM are already there
        with open(target_file, "r") as f:
            content = f.read()

        # Remove existing definitions if any
        lines = content.splitlines(keepends=True)
        new_lines = []
        for line in lines:
            if not line.startswith("CONFIG_VIGO_DTLS_CERT_PEM=") and not line.startswith("CONFIG_VIGO_DTLS_KEY_PEM="):
                new_lines.append(line)

        # Ensure trailing newline in existing content if not empty
        if new_lines and not new_lines[-1].endswith("\n"):
            new_lines[-1] += "\n"

        # Append new lines
        new_lines.extend(lines_to_add)

        with open(target_file, "w") as f:
            f.writelines(new_lines)

        print(f"Successfully generated and updated unique DTLS keys in: {target_file}")
    else:
        # Print to stdout
        sys.stdout.writelines(lines_to_add)

if __name__ == "__main__":
    main()
