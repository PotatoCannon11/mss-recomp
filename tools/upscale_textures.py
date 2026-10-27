#!/usr/bin/env python3
"""upscale_textures.py: turn a texture dump into a replacement pack with Real-ESRGAN (x4plus).

    tools/upscale_textures.py [--dump DIR] [--pack DIR] [--max N] [--skip-from N]

Defaults: dump ~/.local/share/mss-recomp/texdump/RMBE01 (filled by running the game with RT_TEXDUMP=1),
pack ~/.local/share/mss-recomp/textures/RMBE01 (loaded by the game automatically). Textures already in
the pack are skipped, so it can be rerun as the dump grows. Colour goes through the network, alpha is
scaled with Lanczos; edges wrap while upscaling so tiling textures stay seamless. Outputs are capped at
--max pixels per side, and textures already --skip-from wide or tall are left alone.

Needs torch, numpy and pillow; run it with the venv's python:
    ~/.local/share/mss-recomp/upscaler-venv/bin/python tools/upscale_textures.py
"""
import argparse, os, sys, time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image

HOME = os.path.expanduser("~")
BASE = os.path.join(HOME, ".local/share/mss-recomp")


# ---------------------------------------------------------------- RRDBNet (ESRGAN generator)

class RDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.rdb1, self.rdb2, self.rdb3 = RDB(nf, gc), RDB(nf, gc), RDB(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, nb=23, nf=64, gc=32):
        super().__init__()
        self.conv_first = nn.Conv2d(3, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(nb)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, 3, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode="nearest")))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode="nearest")))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


# ---------------------------------------------------------------- upscaling

PAD = 8          # wrap-around context at the edges (input pixels)
TILE = 256       # big inputs go through in tiles of this size (+ padding), to bound memory


@torch.inference_mode()
def esrgan(net, rgb):
    """PIL RGB -> PIL RGB at 4x"""
    w, h = rgb.size
    x = torch.from_numpy(np.asarray(rgb, dtype=np.float32) / 255).permute(2, 0, 1).unsqueeze(0)
    pad = min(PAD, w, h)
    x = F.pad(x, (pad, pad, pad, pad), mode="circular")
    out = torch.zeros(1, 3, h * 4, w * 4)
    for ty in range(0, h, TILE):
        for tx in range(0, w, TILE):
            th, tw = min(TILE, h - ty), min(TILE, w - tx)
            tile = x[:, :, ty:ty + th + 2 * pad, tx:tx + tw + 2 * pad]
            y = net(tile)[:, :, pad * 4:(pad + th) * 4, pad * 4:(pad + tw) * 4]
            out[:, :, ty * 4:(ty + th) * 4, tx * 4:(tx + tw) * 4] = y
    out = out.clamp(0, 1).mul(255).round().byte().squeeze(0).permute(1, 2, 0).numpy()
    return Image.fromarray(np.ascontiguousarray(out), "RGB")


def upscale(net, im, max_side):
    im = im.convert("RGBA")
    w, h = im.size
    rgb = esrgan(net, im.convert("RGB"))
    alpha = im.getchannel("A")
    if alpha.getextrema() == (255, 255):
        alpha = Image.new("L", rgb.size, 255)
    else:
        alpha = alpha.resize(rgb.size, Image.LANCZOS)
    out = Image.merge("RGBA", (*rgb.split(), alpha))
    s = min(1.0, max_side / max(out.size))
    if s < 1:
        out = out.resize((max(1, round(out.width * s)), max(1, round(out.height * s))), Image.LANCZOS)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dump", default=os.path.join(BASE, "texdump/RMBE01"))
    ap.add_argument("--pack", default=os.path.join(BASE, "textures/RMBE01"))
    ap.add_argument("--model", default=os.path.join(BASE, "upscaler/RealESRGAN_x4plus.pth"))
    ap.add_argument("--max", type=int, default=2048, help="largest output side (default 2048)")
    ap.add_argument("--skip-from", type=int, default=1024, help="leave textures this big alone (default 1024)")
    a = ap.parse_args()

    if not os.path.isdir(a.dump):
        sys.exit(f"no dump at {a.dump}: run the game with RT_TEXDUMP=1 first")
    os.makedirs(a.pack, exist_ok=True)
    todo = []
    for f in sorted(os.listdir(a.dump)):
        if not f.startswith("tex1_") or not f.endswith(".png") or os.path.exists(os.path.join(a.pack, f)):
            continue
        w, h = map(int, f.split("_")[1].split("x"))
        if max(w, h) >= a.skip_from or min(w, h) < 4:
            continue
        todo.append((w * h, f))
    todo.sort()
    print(f"{len(todo)} textures to upscale -> {a.pack}")
    if not todo:
        return

    net = RRDBNet()
    sd = torch.load(a.model, map_location="cpu", weights_only=True)
    net.load_state_dict(sd.get("params_ema", sd.get("params", sd)))
    net.eval()

    t0 = time.time()
    for i, (_, f) in enumerate(todo):
        src = os.path.join(a.dump, f)
        try:
            out = upscale(net, Image.open(src), a.max)
        except Exception as e:
            print(f"  {f}: {e}")
            continue
        tmp = os.path.join(a.pack, f + ".part")
        out.save(tmp, "PNG")
        os.replace(tmp, os.path.join(a.pack, f))
        el = time.time() - t0
        print(f"  [{i + 1}/{len(todo)}] {f} -> {out.width}x{out.height}  ({el:.0f}s, ~{el / (i + 1) * (len(todo) - i - 1):.0f}s left)", flush=True)


if __name__ == "__main__":
    main()
