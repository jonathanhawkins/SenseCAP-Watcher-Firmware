#!/usr/bin/env python3
"""
Convert PNG to LVGL C array with proper multi-color-depth support.
Matches the exact format used by LVGL image converter.

LVGL 16-bit TRUE_COLOR_ALPHA format (from docs):
  - Byte 0: Green 3 lower bits, Blue 5 bits (color LOW byte)
  - Byte 1: Red 5 bits, Green 3 higher bits (color HIGH byte)
  - Byte 2: Alpha byte

For LV_COLOR_16_SWAP:
  - Byte 0: Red 5 bits, Green 3 higher bits (color HIGH byte)
  - Byte 1: Green 3 lower bits, Blue 5 bits (color LOW byte)
  - Byte 2: Alpha byte
"""

from PIL import Image
import os

def rgb_to_8bit(r, g, b):
    """Convert RGB to 8-bit R3G3B2 format"""
    r3 = (r >> 5) & 0x07  # 3 bits
    g3 = (g >> 5) & 0x07  # 3 bits
    b2 = (b >> 6) & 0x03  # 2 bits
    return (r3 << 5) | (g3 << 2) | b2

def rgb_to_rgb565(r, g, b):
    """Convert RGB888 to RGB565"""
    r5 = (r >> 3) & 0x1F  # 5 bits
    g6 = (g >> 2) & 0x3F  # 6 bits
    b5 = (b >> 3) & 0x1F  # 5 bits
    # RGB565: RRRRRGGG GGGBBBBB
    return (r5 << 11) | (g6 << 5) | b5

def convert_image(input_path, output_path, var_name):
    """Convert PNG image to LVGL C array format"""

    # Open and convert image to RGBA
    img = Image.open(input_path).convert('RGBA')
    width, height = img.size
    pixels = list(img.getdata())

    pixel_count = width * height

    # Calculate data sizes for each color depth
    data_size_8bit = pixel_count * 2    # 1 byte color + 1 byte alpha
    data_size_16bit = pixel_count * 3   # 2 bytes color + 1 byte alpha
    data_size_32bit = pixel_count * 4   # 4 bytes BGRA

    # Build pixel data for all depths
    data_8bit = []
    data_16bit_noswap = []   # For LV_COLOR_16_SWAP == 0: [color_low, color_high, alpha]
    data_16bit_swap = []     # For LV_COLOR_16_SWAP != 0: [color_high, color_low, alpha]
    data_32bit = []

    for r, g, b, a in pixels:
        # 8-bit: color byte, alpha byte
        color_8 = rgb_to_8bit(r, g, b)
        data_8bit.extend([color_8, a])

        # RGB565 color
        rgb565 = rgb_to_rgb565(r, g, b)
        color_low = rgb565 & 0xFF           # Lower byte: GGGBBBBB
        color_high = (rgb565 >> 8) & 0xFF   # Upper byte: RRRRRGGG

        # 16-bit no swap: [color_low, color_high, alpha]
        data_16bit_noswap.extend([color_low, color_high, a])

        # 16-bit swap: [color_high, color_low, alpha] (bytes swapped)
        data_16bit_swap.extend([color_high, color_low, a])

        # 32-bit BGRA
        data_32bit.extend([b, g, r, a])

    # Generate C file
    c_content = []
    c_content.append('#include "lvgl.h"')
    c_content.append('')
    c_content.append('#ifndef LV_ATTRIBUTE_MEM_ALIGN')
    c_content.append('#define LV_ATTRIBUTE_MEM_ALIGN')
    c_content.append('#endif')
    c_content.append('')
    c_content.append('#ifndef LV_ATTRIBUTE_IMG_' + var_name.upper())
    c_content.append('#define LV_ATTRIBUTE_IMG_' + var_name.upper())
    c_content.append('#endif')
    c_content.append('')
    c_content.append(f'const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_IMG_{var_name.upper()} uint8_t {var_name}_map[] = {{')

    # 8-bit section
    c_content.append('#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8')
    c_content.append('  /*Pixel format: Red: 3 bit, Green: 3 bit, Blue: 2 bit, Alpha 8 bit*/')
    c_content.append(format_bytes(data_8bit))
    c_content.append('')

    # 16-bit non-swapped section
    c_content.append('#elif LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0')
    c_content.append('  /*Pixel format: Blue: 5 bit, Green: 6 bit, Red: 5 bit, Alpha: 8 bit*/')
    c_content.append(format_bytes(data_16bit_noswap))
    c_content.append('')

    # 16-bit swapped section (THIS IS WHAT THE PROJECT USES)
    c_content.append('#elif LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP != 0')
    c_content.append('  /*Pixel format: Blue: 5 bit, Green: 6 bit, Red: 5 bit, Alpha: 8 bit  BUT the 2 color bytes are swapped*/')
    c_content.append(format_bytes(data_16bit_swap))
    c_content.append('')

    # 32-bit section
    c_content.append('#elif LV_COLOR_DEPTH == 32')
    c_content.append('  /*Pixel format: Blue: 8 bit, Green: 8 bit, Red: 8 bit, Alpha: 8 bit*/')
    c_content.append(format_bytes(data_32bit))
    c_content.append('')

    c_content.append('#endif')
    c_content.append('};')
    c_content.append('')

    # Image descriptor struct
    c_content.append(f'const lv_img_dsc_t {var_name} = {{')
    c_content.append('  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,')
    c_content.append('  .header.always_zero = 0,')
    c_content.append('  .header.reserved = 0,')
    c_content.append(f'  .header.w = {width},')
    c_content.append(f'  .header.h = {height},')
    c_content.append('#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8')
    c_content.append(f'  .data_size = {data_size_8bit},')
    c_content.append('#elif LV_COLOR_DEPTH == 16')
    c_content.append(f'  .data_size = {data_size_16bit},')
    c_content.append('#elif LV_COLOR_DEPTH == 32')
    c_content.append(f'  .data_size = {data_size_32bit},')
    c_content.append('#endif')
    c_content.append(f'  .data = {var_name}_map,')
    c_content.append('};')
    c_content.append('')

    # Write file
    with open(output_path, 'w') as f:
        f.write('\n'.join(c_content))

    print(f"Created {output_path}")
    print(f"  Size: {width}x{height}")
    print(f"  Sample black pixel (RGB565=0x0000): low=0x{0 & 0xFF:02x}, high=0x{(0 >> 8) & 0xFF:02x}")

    # Debug: show a sample colored pixel
    for r, g, b, a in pixels:
        if r > 200 or g > 200 or b > 200:  # Find a bright pixel
            rgb565 = rgb_to_rgb565(r, g, b)
            print(f"  Sample bright pixel R={r},G={g},B={b}: RGB565=0x{rgb565:04x}, low=0x{rgb565 & 0xFF:02x}, high=0x{(rgb565 >> 8) & 0xFF:02x}")
            break

def format_bytes(data, bytes_per_line=32):
    """Format byte array as C hex string"""
    lines = []
    for i in range(0, len(data), bytes_per_line):
        chunk = data[i:i+bytes_per_line]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk) + ','
        lines.append('  ' + hex_str)
    return '\n'.join(lines)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_dir = '/Users/light/dev/web-apps/aligned-tools/hardware/watcher-firmware/examples/aligned-realtime/src/ui'

    # Create output directory if needed
    os.makedirs(output_dir, exist_ok=True)

    # Input image
    input_image = os.path.join(script_dir, 'aligned_logo_412x412.png')

    # Generate all animation frames
    frames = [
        ('listening_A', 'listening_A.c'),
        ('listening_B', 'listening_B.c'),
        ('listening_C', 'listening_C.c'),
        ('listening_D', 'listening_D.c'),
        ('listening_E', 'listening_E.c'),
        ('speaking_A', 'speaking_A.c'),
        ('speaking_B', 'speaking_B.c'),
        ('speaking_C', 'speaking_C.c'),
        ('speaking_D', 'speaking_D.c'),
        ('speaking_E', 'speaking_E.c'),
    ]

    for var_name, filename in frames:
        output_path = os.path.join(output_dir, filename)
        convert_image(input_image, output_path, var_name)

if __name__ == '__main__':
    main()
