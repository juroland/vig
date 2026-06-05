#!/usr/bin/env python3
import os
import sys
import urllib.request
import tempfile

PEDESTRIAN_URL = "https://raw.githubusercontent.com/pjreddie/darknet/master/data/person.jpg"
NON_PEDESTRIAN_URL = "https://raw.githubusercontent.com/pjreddie/darknet/master/data/giraffe.jpg"

TARGET_W = 448
TARGET_H = 448

def download_image(url):
    print(f"Downloading {url}...")
    temp_file = tempfile.NamedTemporaryFile(delete=False, suffix=".jpg")
    urllib.request.urlretrieve(url, temp_file.name)
    return temp_file.name

def convert_to_ouyy_evyy(img_path):
    img = Image.open(img_path).convert("RGB")
    # Resize to TARGET_W x TARGET_H
    img = img.resize((TARGET_W, TARGET_H), Image.Resampling.LANCZOS)
    
    # Get raw RGB pixels
    pixels = list(img.getdata()) # list of (R, G, B)
    
    # Calculate Y, U, V
    y_vals = []
    u_vals = []
    v_vals = []
    for r, g, b in pixels:
        # Standard ITU-R BT.601 conversion
        y = 0.299 * r + 0.587 * g + 0.114 * b
        u = -0.1687 * r - 0.3313 * g + 0.5 * b + 128
        v = 0.5 * r - 0.4187 * g - 0.0813 * b + 128
        
        # Clamp to 0-255
        y_vals.append(max(0, min(255, int(y))))
        u_vals.append(max(0, min(255, int(u))))
        v_vals.append(max(0, min(255, int(v))))

    # Encode to OUYY_EVYY format
    # Buffer size: TARGET_W * TARGET_H * 3 / 2
    # Each row has TARGET_W * 1.5 bytes.
    # For each macro-pixel (2 pixels horizontally):
    # Byte 0: Chroma (U on even rows, V on odd rows)
    # Byte 1: Y0
    # Byte 2: Y1
    encoded = bytearray()
    for y in range(TARGET_H):
        row_offset = y * TARGET_W
        for x in range(0, TARGET_W, 2):
            # Indices for pixel 0 and pixel 1 in this macro-pixel
            idx0 = row_offset + x
            idx1 = row_offset + x + 1
            
            # Chroma values
            u0, u1 = u_vals[idx0], u_vals[idx1]
            v0, v1 = v_vals[idx0], v_vals[idx1]
            avg_u = (u0 + u1) // 2
            avg_v = (v0 + v1) // 2
            
            chroma = avg_u if (y % 2 == 0) else avg_v
            
            encoded.append(chroma)
            encoded.append(y_vals[idx0])
            encoded.append(y_vals[idx1])
            
    return encoded

def write_header(name, data, output_path):
    print(f"Writing {output_path}...")
    with open(output_path, "w") as f:
        f.write(f"// Generated test image: {name}\n")
        f.write(f"// Width: {TARGET_W}, Height: {TARGET_H}\n")
        f.write(f"// Size: {len(data)} bytes\n")
        f.write("#pragma once\n\n")
        f.write("#include <cstdint>\n\n")
        f.write(f"static const uint8_t {name}_image_data[{len(data)}] = {{\n")
        
        # Format as hex bytes, 16 per line
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_strs = [f"0x{b:02x}" for b in chunk]
            f.write("    " + ", ".join(hex_strs) + ",\n")
            
        f.write("};\n")

def main():
    # Make sure target directory exists
    out_dir = "tests/unit/main"
    os.makedirs(out_dir, exist_ok=True)
    
    # Download and process pedestrian
    ped_path = download_image(PEDESTRIAN_URL)
    ped_data = convert_to_ouyy_evyy(ped_path)
    write_header("pedestrian", ped_data, os.path.join(out_dir, "pedestrian_image.h"))
    os.remove(ped_path)
    
    # Download and process non-pedestrian
    non_ped_path = download_image(NON_PEDESTRIAN_URL)
    non_ped_data = convert_to_ouyy_evyy(non_ped_path)
    write_header("non_pedestrian", non_ped_data, os.path.join(out_dir, "non_pedestrian_image.h"))
    os.remove(non_ped_path)
    
    print("Test images generated successfully.")

if __name__ == "__main__":
    main()
