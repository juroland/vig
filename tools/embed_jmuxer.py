#!/usr/bin/env python3
import sys
import os


def embed_jmuxer(template_file, jmuxer_file, output_file):
    with open(template_file, "r", encoding="utf-8") as f:
        template = f.read()

    with open(jmuxer_file, "r", encoding="utf-8") as f:
        jmuxer_content = f.read()

    # Replace placeholder with jmuxer content
    result = template.replace("/* JMUXER_PLACEHOLDER */", jmuxer_content)

    # Ensure parent directory of output_file exists
    os.makedirs(os.path.dirname(output_file), exist_ok=True)

    with open(output_file, "w", encoding="utf-8") as f:
        f.write(result)


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: embed_jmuxer.py <template_h> <jmuxer_js> <output_h>")
        sys.exit(1)
    embed_jmuxer(sys.argv[1], sys.argv[2], sys.argv[3])
