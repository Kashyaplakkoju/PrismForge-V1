"""
build_models.py
Master reproducible build script for the original 3D face filters:
- Generates 2048x2048 PBR texture atlases
- Builds neutral adult reference head (~0.16m temple width)
- Builds Aurora Fox Mask (.blend + .glb)
- Builds Nocturne Owl Mask (.blend + .glb)
- Builds Atlas Explorer Helmet (.blend + .glb)
Can be run with system Python (invoking Blender headless) or called directly from tests.
"""
import os
import sys
import subprocess

BLENDER_EXE = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"

def run_cmd(cmd):
    print(f"Running: {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Error output:\n{res.stderr}")
        raise RuntimeError(f"Command failed with code {res.returncode}")
    print(res.stdout)

def main():
    sources_dir = os.path.dirname(os.path.abspath(__file__))
    deliveries_dir = os.path.dirname(sources_dir)
    textures_dir = os.path.join(sources_dir, "textures")
    models_dir = os.path.join(deliveries_dir, "models")

    os.makedirs(textures_dir, exist_ok=True)
    os.makedirs(models_dir, exist_ok=True)

    print("=== Step 1: Generating PBR Textures ===")
    from generate_textures import (
        create_aurora_fox_textures,
        create_nocturne_owl_textures,
        create_atlas_helmet_textures
    )
    create_aurora_fox_textures(textures_dir)
    create_nocturne_owl_textures(textures_dir)
    create_atlas_helmet_textures(textures_dir)

    print("=== Step 2: Generating Neutral Reference Head ===")
    head_script = os.path.join(sources_dir, "generate_reference_head.py")
    run_cmd([BLENDER_EXE, "--background", "--python", head_script])

    print("=== Step 3: Generating Aurora Fox Mask ===")
    fox_script = os.path.join(sources_dir, "generate_aurora_fox.py")
    run_cmd([BLENDER_EXE, "--background", "--python", fox_script])

    print("=== Step 4: Generating Nocturne Owl Mask ===")
    owl_script = os.path.join(sources_dir, "generate_nocturne_owl.py")
    run_cmd([BLENDER_EXE, "--background", "--python", owl_script])

    print("=== Step 5: Generating Atlas Explorer Helmet ===")
    helmet_script = os.path.join(sources_dir, "generate_atlas_helmet.py")
    run_cmd([BLENDER_EXE, "--background", "--python", helmet_script])

    print("=== All models generated successfully! ===")

if __name__ == "__main__":
    main()
