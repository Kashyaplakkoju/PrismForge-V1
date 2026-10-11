from pathlib import Path
import sys
root=Path(__file__).resolve().parent
sys.path.insert(0,str(root))
from generate_textures import create_nocturne_owl_textures,create_aurora_fox_textures
create_nocturne_owl_textures(str(root/"textures"))
create_aurora_fox_textures(str(root/"textures"))
