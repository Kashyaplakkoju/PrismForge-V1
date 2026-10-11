"""
generate_textures.py
Generates original, high-resolution 2048x2048 PBR texture atlases
for Aurora Fox Mask, Nocturne Owl Mask, and Atlas Explorer Helmet.
Outputs embedded-ready PNGs conforming to standard glTF metallic/roughness PBR.
"""
import os
import math
from PIL import Image, ImageDraw, ImageFilter

def create_aurora_fox_textures(output_dir):
    width, height = 2048, 2048
    os.makedirs(output_dir, exist_ok=True)

    # 1. Albedo Map
    albedo = Image.new("RGBA", (width, height), (246, 248, 252, 255)) # Porcelain white base
    draw = ImageDraw.Draw(albedo)

    # Subtle porcelain gradient / shading across the face area
    for y in range(height):
        ratio = y / height
        # subtle warm shading toward base
        tint = int(246 - ratio * 14)
        draw.line([(0, y), (width, y)], fill=(tint, tint + 2, tint + 6, 255))

    # Traditional vermilion ceremonial markings
    # Symmetrical eye liner wings
    crimson = (196, 26, 26, 255)
    deep_crimson = (150, 16, 16, 255)
    gold = (212, 175, 55, 255)
    charcoal = (32, 33, 38, 255)

    # Left eye surround & wing (centered around x=650, y=1050)
    # Right eye surround & wing (centered around x=1398, y=1050)
    eye_lx, eye_rx = 640, 1408
    eye_y = 1050

    # Draw eye surround wing shapes
    for eye_cx, sign in [(eye_lx, -1), (eye_rx, 1)]:
        # Broad outer red wing sweep
        wing_poly = [
            (eye_cx - sign * 140, eye_y - 30),
            (eye_cx, eye_y - 90),
            (eye_cx + sign * 260, eye_y - 140), # sweeping up toward temple
            (eye_cx + sign * 320, eye_y - 80),
            (eye_cx + sign * 180, eye_y + 10),
            (eye_cx + sign * 80, eye_y + 70),
            (eye_cx, eye_y + 50),
            (eye_cx - sign * 100, eye_y + 20),
        ]
        draw.polygon(wing_poly, fill=crimson)

        # Inner golden filigree stroke along the brow edge
        gold_stroke = [
            (eye_cx - sign * 100, eye_y - 70),
            (eye_cx, eye_y - 100),
            (eye_cx + sign * 240, eye_y - 150),
            (eye_cx + sign * 290, eye_y - 100),
            (eye_cx + sign * 220, eye_y - 110),
            (eye_cx, eye_y - 80),
        ]
        draw.polygon(gold_stroke, fill=gold)

        # Cheek whisker flourishes (3 curving ribbons)
        for i, offset in enumerate([60, 140, 220]):
            curve = [
                (eye_cx + sign * 80, eye_y + 120 + offset * 0.4),
                (eye_cx + sign * 220, eye_y + 150 + offset * 0.6),
                (eye_cx + sign * 340, eye_y + 200 + offset * 0.9),
                (eye_cx + sign * 330, eye_y + 230 + offset * 0.9),
                (eye_cx + sign * 200, eye_y + 180 + offset * 0.6),
                (eye_cx + sign * 70, eye_y + 140 + offset * 0.4),
            ]
            draw.polygon(curve, fill=crimson)

    # Forehead central crest (Magatama / Kitsune flame emblem)
    crest_cx = 1024
    crest_cy = 620
    crest_flame = [
        (crest_cx, crest_cy - 220), # top spire
        (crest_cx + 70, crest_cy - 120),
        (crest_cx + 110, crest_cy - 20),
        (crest_cx + 60, crest_cy + 90),
        (crest_cx, crest_cy + 130), # base
        (crest_cx - 60, crest_cy + 90),
        (crest_cx - 110, crest_cy - 20),
        (crest_cx - 70, crest_cy - 120),
    ]
    draw.polygon(crest_flame, fill=crimson)

    # Inner golden core in forehead crest
    inner_flame = [
        (crest_cx, crest_cy - 150),
        (crest_cx + 40, crest_cy - 60),
        (crest_cx + 25, crest_cy + 40),
        (crest_cx, crest_cy + 70),
        (crest_cx - 25, crest_cy + 40),
        (crest_cx - 40, crest_cy - 60),
    ]
    draw.polygon(inner_flame, fill=gold)

    # Fox nose pad at bottom center
    nose_poly = [
        (crest_cx - 60, 1580),
        (crest_cx + 60, 1580),
        (crest_cx + 45, 1660),
        (crest_cx, 1690),
        (crest_cx - 45, 1660),
    ]
    draw.polygon(nose_poly, fill=charcoal)

    # Ear tips and inner ear tufts (quadrants at top)
    for ear_cx, sign in [(450, -1), (1598, 1)]:
        # Ear tip crimson gradient patch
        draw.polygon([
            (ear_cx, 60),
            (ear_cx + sign * 180, 180),
            (ear_cx + sign * 140, 360),
            (ear_cx - sign * 120, 320),
            (ear_cx - sign * 160, 160),
        ], fill=crimson)
        # Inner ear warm rosy shading
        draw.polygon([
            (ear_cx, 160),
            (ear_cx + sign * 90, 240),
            (ear_cx + sign * 60, 360),
            (ear_cx - sign * 70, 340),
        ], fill=(245, 230, 224, 255))
        # Gold ear rim accent
        draw.line([(ear_cx - sign * 150, 180), (ear_cx, 70), (ear_cx + sign * 160, 200)], fill=gold, width=18)

    # Smooth the painted strokes slightly for natural lacquer appearance
    albedo = albedo.filter(ImageFilter.GaussianBlur(radius=2))

    # 2. Roughness & Metallic Map (Green = Roughness, Blue = Metallic)
    # Porcelain: Roughness 0.24 (61/255), Metalness 0.02 (5/255)
    # Crimson lacquer: Roughness 0.32 (82/255), Metalness 0.04 (10/255)
    # Gold leaf: Roughness 0.16 (41/255), Metalness 0.88 (224/255)
    # Charcoal nose pad: Roughness 0.58 (148/255), Metalness 0.0 (0/255)
    rough_metal = Image.new("RGBA", (width, height), (0, 61, 5, 255)) # Default ceramic
    rm_draw = ImageDraw.Draw(rough_metal)

    # Red areas
    for eye_cx, sign in [(eye_lx, -1), (eye_rx, 1)]:
        wing_poly = [
            (eye_cx - sign * 140, eye_y - 30),
            (eye_cx, eye_y - 90),
            (eye_cx + sign * 260, eye_y - 140),
            (eye_cx + sign * 320, eye_y - 80),
            (eye_cx + sign * 180, eye_y + 10),
            (eye_cx + sign * 80, eye_y + 70),
            (eye_cx, eye_y + 50),
            (eye_cx - sign * 100, eye_y + 20),
        ]
        rm_draw.polygon(wing_poly, fill=(0, 82, 10, 255))
        gold_stroke = [
            (eye_cx - sign * 100, eye_y - 70),
            (eye_cx, eye_y - 100),
            (eye_cx + sign * 240, eye_y - 150),
            (eye_cx + sign * 290, eye_y - 100),
            (eye_cx + sign * 220, eye_y - 110),
            (eye_cx, eye_y - 80),
        ]
        rm_draw.polygon(gold_stroke, fill=(0, 41, 224, 255))

    rm_draw.polygon(crest_flame, fill=(0, 82, 10, 255))
    rm_draw.polygon(inner_flame, fill=(0, 41, 224, 255))
    rm_draw.polygon(nose_poly, fill=(0, 148, 0, 255))

    albedo_path = os.path.join(output_dir, "aurora_fox_albedo.png")
    rm_path = os.path.join(output_dir, "aurora_fox_roughness_metal.png")
    albedo.save(albedo_path, format="PNG")
    rough_metal.save(rm_path, format="PNG")
    print(f"Generated Aurora Fox textures: {albedo_path}, {rm_path}")

def create_nocturne_owl_textures(output_dir):
    width, height = 2048, 2048
    os.makedirs(output_dir, exist_ok=True)

    # 1. Albedo Map
    albedo = Image.new("RGBA", (width, height), (28, 31, 38, 255)) # Midnight obsidian slate
    draw = ImageDraw.Draw(albedo)

    bronze = (125, 98, 60, 255)
    amber_gold = (191, 160, 84, 255)
    burnished_beak = (84, 68, 50, 255)
    charcoal_shadow = (18, 20, 24, 255)

    # Concentric radiating plumage rings on facial discs
    eye_lx, eye_rx = 640, 1408
    eye_y = 1000

    for eye_cx in [eye_lx, eye_rx]:
        # Radiating plumage rings
        for r in range(480, 120, -35):
            angle_steps = 36
            ring_color = bronze if (r // 35) % 2 == 0 else amber_gold
            for a_idx in range(angle_steps):
                a1 = (a_idx / angle_steps) * 2 * math.pi
                a2 = ((a_idx + 0.6) / angle_steps) * 2 * math.pi
                x1 = eye_cx + r * math.cos(a1)
                y1 = eye_y + (r * 0.85) * math.sin(a1)
                x2 = eye_cx + (r - 28) * math.cos(a2)
                y2 = eye_y + ((r - 28) * 0.85) * math.sin(a2)
                draw.line([(x1, y1), (x2, y2)], fill=ring_color, width=9)

        # Dark velvety eye cavity rim
        draw.ellipse([eye_cx - 130, eye_y - 110, eye_cx + 130, eye_y + 110], fill=charcoal_shadow, outline=amber_gold, width=8)

    # Plumicorn (ear horn) feather quills
    for horn_cx, sign in [(520, -1), (1528, 1)]:
        for i in range(8):
            py = 180 + i * 50
            draw.line([(horn_cx, py), (horn_cx + sign * (120 - i * 8), py - 70)], fill=amber_gold, width=12)
            draw.line([(horn_cx + sign * 20, py + 10), (horn_cx + sign * (130 - i * 8), py - 60)], fill=bronze, width=8)

    # Beak area at center bottom
    beak_poly = [
        (960, 1160),
        (1088, 1160),
        (1060, 1560),
        (1024, 1680), # hooked sharp tip
        (988, 1560),
    ]
    draw.polygon(beak_poly, fill=burnished_beak, outline=amber_gold)

    # Horizontal barred plumage stripes across cheeks and brow
    for cy in range(400, 800, 35):
        draw.line([(400, cy), (1648, cy)], fill=(45, 50, 60, 160), width=12)
    for cy in range(1250, 1800, 40):
        draw.line([(350, cy), (800, cy)], fill=bronze, width=10)
        draw.line([(1248, cy), (1698, cy)], fill=bronze, width=10)

    albedo = albedo.filter(ImageFilter.GaussianBlur(radius=2))

    # 2. Roughness & Metallic Map
    # Plumage: Roughness 0.72 (184/255), Metalness 0.08 (20/255)
    # Beak / Horn: Roughness 0.28 (71/255), Metalness 0.60 (153/255)
    # Amber/Gold accents: Roughness 0.22 (56/255), Metalness 0.82 (209/255)
    rough_metal = Image.new("RGBA", (width, height), (0, 184, 20, 255))
    rm_draw = ImageDraw.Draw(rough_metal)

    rm_draw.polygon(beak_poly, fill=(0, 71, 153, 255))
    for eye_cx in [eye_lx, eye_rx]:
        rm_draw.ellipse([eye_cx - 140, eye_y - 120, eye_cx + 140, eye_y + 120], fill=(0, 56, 209, 255))

    albedo_path = os.path.join(output_dir, "nocturne_owl_albedo.png")
    rm_path = os.path.join(output_dir, "nocturne_owl_roughness_metal.png")
    albedo.save(albedo_path, format="PNG")
    rough_metal.save(rm_path, format="PNG")
    print(f"Generated Nocturne Owl textures: {albedo_path}, {rm_path}")

def create_atlas_helmet_textures(output_dir):
    width, height = 2048, 2048
    os.makedirs(output_dir, exist_ok=True)

    # 1. Albedo Map
    albedo = Image.new("RGBA", (width, height), (226, 230, 236, 255)) # Aerospace composite white
    draw = ImageDraw.Draw(albedo)

    graphite = (48, 52, 60, 255)
    titanium = (112, 100, 84, 255)
    hazard_amber = (217, 126, 36, 255)
    seal_black = (24, 25, 28, 255)

    # Structural panel division lines
    for x in [400, 800, 1248, 1648]:
        draw.line([(x, 100), (x, 1900)], fill=(185, 192, 202, 255), width=8)
    for y in [450, 950, 1450]:
        draw.line([(100, y), (1948, y)], fill=(185, 192, 202, 255), width=8)

    # Panoramic Visor Frame & Hermetic Gasket (Central region)
    # Heavy rubber gasket outline around visor portal
    visor_box = [340, 680, 1708, 1420]
    draw.rounded_rectangle(visor_box, radius=90, outline=seal_black, width=48)
    # Visor clamp brackets
    for vx in [450, 750, 1024, 1298, 1598]:
        for vy in [680, 1420]:
            draw.rectangle([vx - 30, vy - 24, vx + 30, vy + 24], fill=titanium, outline=graphite, width=4)

    # Lateral Comms Pods (Temples)
    for pod_cx in [250, 1798]:
        draw.ellipse([pod_cx - 160, 750, pod_cx + 160, 1070], fill=graphite, outline=titanium, width=16)
        # Inner intake grille
        for gy in range(790, 1030, 24):
            draw.line([(pod_cx - 100, gy), (pod_cx + 100, gy)], fill=seal_black, width=8)

    # Lower Cheek Rebreather Regulators & Hazard Strips
    for reg_cx in [480, 1568]:
        draw.rectangle([reg_cx - 140, 1520, reg_cx + 140, 1840], fill=titanium, outline=graphite, width=12)
        # Hazard amber chevrons
        for cy in range(1560, 1800, 48):
            draw.polygon([
                (reg_cx - 120, cy),
                (reg_cx, cy + 24),
                (reg_cx + 120, cy),
                (reg_cx + 120, cy + 18),
                (reg_cx, cy + 42),
                (reg_cx - 120, cy + 18),
            ], fill=hazard_amber)

    # Collar Neck Ring Flange (Bottom region)
    draw.rectangle([100, 1880, 1948, 2000], fill=graphite, outline=titanium, width=14)
    # Latch locking lugs
    for lx in range(250, 1850, 200):
        draw.rectangle([lx - 25, 1900, lx + 25, 1980], fill=titanium, outline=hazard_amber, width=6)

    # 2. Roughness & Metallic Map
    # Composite shell: Roughness 0.36 (92/255), Metalness 0.08 (20/255)
    # Titanium canisters/latches: Roughness 0.22 (56/255), Metalness 0.85 (217/255)
    # Rubber gasket/conduits: Roughness 0.82 (209/255), Metalness 0.02 (5/255)
    rough_metal = Image.new("RGBA", (width, height), (0, 92, 20, 255))
    rm_draw = ImageDraw.Draw(rough_metal)

    # Rubber gasket
    rm_draw.rounded_rectangle(visor_box, radius=90, outline=(0, 209, 5, 255), width=48)
    # Titanium areas
    for vx in [450, 750, 1024, 1298, 1598]:
        for vy in [680, 1420]:
            rm_draw.rectangle([vx - 30, vy - 24, vx + 30, vy + 24], fill=(0, 56, 217, 255))
    for pod_cx in [250, 1798]:
        rm_draw.ellipse([pod_cx - 160, 750, pod_cx + 160, 1070], fill=(0, 56, 217, 255))
    for reg_cx in [480, 1568]:
        rm_draw.rectangle([reg_cx - 140, 1520, reg_cx + 140, 1840], fill=(0, 56, 217, 255))

    albedo_path = os.path.join(output_dir, "atlas_helmet_albedo.png")
    rm_path = os.path.join(output_dir, "atlas_helmet_roughness_metal.png")
    albedo.save(albedo_path, format="PNG")
    rough_metal.save(rm_path, format="PNG")
    print(f"Generated Atlas Helmet textures: {albedo_path}, {rm_path}")

if __name__ == "__main__":
    tex_dir = str((__import__("pathlib").Path(__file__).resolve().parent / "../sources/textures").resolve())
    create_aurora_fox_textures(tex_dir)
    create_nocturne_owl_textures(tex_dir)
    create_atlas_helmet_textures(tex_dir)
    print("All textures generated successfully.")
