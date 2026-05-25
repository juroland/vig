#!/usr/bin/env python3
import sys
import os
import re


def embed_html(input_file, output_file):
    base_dir = os.path.dirname(input_file)
    with open(input_file, "r", encoding="utf-8") as f:
        content = f.read()

    # 1. Inline scripts from libs/
    def replace_script(match):
        src = match.group(1)
        if src.startswith("libs/"):
            lib_path = os.path.join(base_dir, src)
            if os.path.exists(lib_path):
                print(f"Inlining {src}...")
                with open(lib_path, "r", encoding="utf-8") as lf:
                    lib_content = lf.read()
                # If it's Decoder.js, we need to handle the worker blob
                if "Decoder.js" in src:
                    # We wrap it in a script tag but also store it for the blob
                    # We create the Blob URL immediately so that it is available when Player is initialized
                    return (
                        f'<script id="decoder-js-content" type="text/plain">{lib_content}</script>\n'
                        f"<script>{lib_content}</script>\n"
                        f"<script>\n"
                        f"  (function() {{\n"
                        f'    const decoderElem = document.getElementById("decoder-js-content");\n'
                        f"    if (decoderElem) {{\n"
                        f'      const blob = new Blob([decoderElem.textContent], {{ type: "application/javascript" }});\n'
                        f"      window.BROADWAY_WORKER_URL = URL.createObjectURL(blob);\n"
                        f'      console.log("Created Broadway worker Blob URL", window.BROADWAY_WORKER_URL);\n'
                        f"    }}\n"
                        f"  }})();\n"
                        f"</script>"
                    )
                return f"<script>{lib_content}</script>"
        return match.group(0)

    content = re.sub(r'<script src="([^"]+)"></script>', replace_script, content)

    # 2. Update the workerFile reference in the script
    content = content.replace(
        "let workerUrl = 'libs/Decoder.js';",
        "let workerUrl = window.BROADWAY_WORKER_URL || 'libs/Decoder.js';",
    )

    # Escape special characters for C string
    escaped = content.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

    with open(output_file, "w", encoding="utf-8") as f:
        f.write("#pragma once\n\n")
        f.write('static const char stream_page_html[] = "')
        f.write(escaped)
        f.write('";\n')


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: embed_html.py <input_html> <output_header>")
        sys.exit(1)
    embed_html(sys.argv[1], sys.argv[2])
