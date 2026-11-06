#!/usr/bin/env python3
"""Turns a recording's capture folder into something to look at, offline. Reads the folder, never changes what is there.

    python tools/capture_pack.py <folder> [--out <folder>] [--compare <other folder>]

Writes, beside the capture unless --out says otherwise:
  Screenshot lifted.png  the screenshot with its darks raised (gamma 0.45), to read shadows
  Capture sheet.png      one image with what the folder has: screenshot, lifted, the frames after the recording (AO alone, classes
                         with their legend, white albedo), the probed pixels, video frames, cube strips, the largest map, text panels
  Video.avi              Video.mjpeg in a file players open, when ffmpeg is there (the picture is copied, not re-encoded)
  Comparison.png         with --compare: both lifted screenshots and their difference x4; refused when the cameras differ
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

GAMMA = 0.45
TILE = (960, 540)
EYE_TOLERANCE = 0.01  # metres
AXIS_TOLERANCE = 0.001  # the camera's direction, as unit vectors


def lifted(image):
    lut = [round(255 * (i / 255) ** GAMMA) for i in range(256)]
    return image.convert('RGB').point(lut * 3)


def read_recording(folder):
    file = folder / 'Recording.txt'
    return file.read_text(encoding='utf-8', errors='replace').splitlines() if file.exists() else []


def tagged(lines, tag):
    """The text after ' [tag] ' of the timeline's lines, in order (the header's [video] line has no clock before it)."""
    mark = f'[{tag}] '
    found = []
    for line in lines:
        if line.startswith('==== ApexRadiance.toml'):
            break
        at = line.find(mark)
        if at == 0 or (at == 13 and line[2] == ':'):
            found.append(line[at + len(mark):])
    return found


def parse_camera(text):
    """A [camera] line: (mirrored, near, eye or None, the 4 rows of the view-projection)."""
    m = re.fullmatch(r'(plain|mirrored) near (\S+)(?: eye (\S+) (\S+) (\S+))? vp((?: \S+){16})', text)
    if not m:
        return None
    vp = [float(v) for v in m.group(6).split()]
    eye = [float(m.group(i)) for i in (3, 4, 5)] if m.group(3) else None
    return m.group(1) == 'mirrored', float(m.group(2)), eye, [vp[0:4], vp[4:8], vp[8:12], vp[12:16]]


def view_axis(camera):
    """The direction the camera looks in, the same for a camera and its mirrored copy (which has world Y negated)."""
    mirrored, _, _, vp = camera
    x, y, z = vp[3][0], vp[3][1] * (-1 if mirrored else 1), vp[3][2]
    length = (x * x + y * y + z * z) ** 0.5 or 1.0
    return x / length, y / length, z / length


def camera_difference(a, b):
    """Why two cameras are not the same view, or None. Without the game's eye the translations of the rows are compared."""
    if abs(a[1] - b[1]) > 0.002:
        return f'near plane {a[1]} and {b[1]}'
    apart = max(abs(p - q) for p, q in zip(view_axis(a), view_axis(b)))
    if apart > AXIS_TOLERANCE:
        return f'looking {apart:.4f} apart'
    if a[2] and b[2]:
        apart = max(abs(p - q) for p, q in zip(a[2], b[2]))
        if apart > EYE_TOLERANCE:
            return f'eyes {apart:.3f} m apart'
    elif a[0] != b[0] or max(abs(p - q) for r, s in zip(a[3], b[3]) for p, q in zip(r, s)) > 1e-3:
        return 'view-projections differ and the eye is unknown'
    return None


def last_camera(folder):
    cameras = tagged(read_recording(folder), 'camera')
    return parse_camera(cameras[-1]) if cameras else None


def split_jpegs(data):
    """The JPEGs of a Video.mjpeg: each starts with FF D8 FF, which entropy-coded data cannot hold (FF is stuffed with 00)."""
    starts = [m.start() for m in re.finditer(b'\xff\xd8\xff', data)]
    return [data[a:b] for a, b in zip(starts, starts[1:] + [len(data)])]


def video_frames(folder, count=3):
    file = folder / 'Video.mjpeg'
    if not file.exists():
        return []
    frames = split_jpegs(file.read_bytes())
    if not frames:
        return []
    from io import BytesIO
    picks = sorted({round(i * (len(frames) - 1) / max(count - 1, 1)) for i in range(count)})
    return [(f'Video frame {i} of {len(frames)}', Image.open(BytesIO(frames[i])).convert('RGB')) for i in picks]


def font(size):
    try:
        return ImageFont.truetype('consola.ttf', size)
    except OSError:
        return ImageFont.load_default(size)


def fit(image):
    """The image inside a tile, keeping its shape."""
    tile = Image.new('RGB', TILE, (24, 24, 28))
    scale = min(TILE[0] / image.width, TILE[1] / image.height)
    small = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))), Image.LANCZOS)
    tile.paste(small, ((TILE[0] - small.width) // 2, (TILE[1] - small.height) // 2))
    return tile


def cube_tile(cubes):
    """[cube] lines as swatches: a row of 6 faces for each probe, lifted as the screenshot is. None when there are none."""
    rows = []
    for text in cubes:
        faces = re.findall(r'\| (\S+) (\S+) (\S+)', text)
        if len(faces) == 6:
            rows.append((text.split(',')[0], [tuple(round(255 * (float(v) / 255) ** GAMMA) for v in f) for f in faces]))
    if not rows:
        return None
    tile = Image.new('RGB', TILE, (24, 24, 28))
    draw = ImageDraw.Draw(tile)
    height = max(10, min(44, (TILE[1] - 10) // len(rows)))
    for i, (name, faces) in enumerate(rows[:(TILE[1] - 10) // height]):
        y = 5 + i * height
        for k, colour in enumerate(faces):
            draw.rectangle((540 + k * 68, y, 604 + k * 68, y + height - 3), fill=colour)
        if height >= 14:
            draw.text((8, y), name, font=font(13), fill=(220, 220, 225))
    return tile


def read_burst(folder):
    file = folder / 'Burst.txt'
    return file.read_text(encoding='utf-8', errors='replace').splitlines() if file.exists() else []


def strips_tile(files):
    """Cubes\\*.png, a row each (faces +x -x +y -y +z -z), lifted. None when there are none."""
    if not files:
        return None
    tile = Image.new('RGB', TILE, (24, 24, 28))
    draw = ImageDraw.Draw(tile)
    height = max(16, min(120, (TILE[1] - 10) // len(files)))
    for i, file in enumerate(files[:(TILE[1] - 10) // height]):
        y = 5 + i * height
        strip = lifted(Image.open(file))
        width = min(720, strip.width * (height - 3) // strip.height)
        tile.paste(strip.resize((width, height - 3), Image.NEAREST), (TILE[0] - width - 6, y))
        draw.text((8, y), file.stem, font=font(13), fill=(220, 220, 225))
    return tile


def classes_tile(image, burst):
    """The classes frame with its legend: the [class] lines of Burst.txt."""
    tile = fit(image)
    draw = ImageDraw.Draw(tile)
    rows = [[part.strip() for part in text.split('|')] for text in tagged(burst, 'class')]
    for i, (name, colour, pixels) in enumerate(row for row in rows if len(row) == 3):
        y = 6 + i * 18
        draw.rectangle((6, y, 300, y + 16), fill=(0, 0, 0))
        draw.rectangle((8, y + 2, 20, y + 14), fill=tuple(int(v) for v in colour.split()))
        draw.text((26, y + 1), f'{name}: {int(pixels):,} px', font=font(13), fill=(255, 255, 255))
    return tile


def probed_tile(image, burst):
    """The lifted screenshot with the light captures' pixels marked. None when the burst has none."""
    marks = [m for m in (re.fullmatch(r'Light capture (\S+): pixel (\d+) (\d+), .*', t) for t in tagged(burst, 'probe')) if m]
    if not marks:
        return None
    tile = fit(lifted(image))
    draw = ImageDraw.Draw(tile)
    scale = min(TILE[0] / image.width, TILE[1] / image.height)
    left, top = (TILE[0] - image.width * scale) / 2, (TILE[1] - image.height * scale) / 2
    for mark in marks:
        x, y = left + int(mark.group(2)) * scale, top + int(mark.group(3)) * scale
        draw.ellipse((x - 9, y - 9, x + 9, y + 9), outline=(255, 255, 0), width=2)
        draw.text((x + 12, y - 8), mark.group(1), font=font(14), fill=(255, 255, 0))
    return tile


def largest_map(folder):
    files = sorted((folder / 'Maps').glob('*.png'), key=lambda f: f.stat().st_size, reverse=True)
    files = [f for f in files if not f.stem.endswith('_alpha')]
    return files[0] if files else None


def lines_tile(lines):
    tile = Image.new('RGB', TILE, (24, 24, 28))
    draw = ImageDraw.Draw(tile)
    y = 8
    for line in lines:
        while line and y < TILE[1] - 18:
            draw.text((8, y), line[:118], font=font(13), fill=(235, 235, 240))
            line = line[118:]
            y += 17
    return tile


def capture_summary(file):
    """A Light capture.txt in two lines: what it measured and the colour on screen."""
    if not file.exists():
        return []
    text = file.read_text(encoding='utf-8', errors='replace').splitlines()
    return [file.parent.name + ': ' + line for line in text[:1] + [t for t in text if t.startswith('== COR NA TELA')]]


def burst_tile(folder, lines, burst):
    """Burst.txt without its legend rows, what the light captures and the snapshot say, and the lamp parts of the rigs."""
    text = [line for line in burst if not line.startswith('[class] ')]
    for name in ('Light capture mouse', 'Light capture cube'):
        text += capture_summary(folder / name / 'Light capture.txt')
    snapshot = folder / 'Lighting snapshot.txt'
    if snapshot.exists():
        text += [t for t in snapshot.read_text(encoding='utf-8', errors='replace').splitlines() if t.startswith('End: ')]
    furniture = tagged(lines, 'furniture')
    boxed = sum(' | box ' in t for t in furniture)
    if boxed:
        text.append(f'Rigs: {boxed} [furniture] lines with a box, {sum("lamp part" in t for t in furniture)} lamp parts, '
                    f'{sum(" | rig own" in t for t in furniture)} with the rig\'s own lights')
    return lines_tile(text)


def text_tile(lines, folder):
    head = [folder.name]
    for line in lines[:30]:
        if line.startswith(('Apex Radiance recording', 'WARNING', 'Frame time', '[video]')):
            head.append(line)
    effects = tagged(lines, 'effect')
    ran = tagged(lines, 'ran')
    settle = tagged(lines, 'settle')
    cameras = [parse_camera(t) for t in tagged(lines, 'camera')]
    if cameras and cameras[-1]:
        head.append(f'Camera at the end: {"MIRRORED" if cameras[-1][0] else "plain"}, near {cameras[-1][1]}, eye {cameras[-1][2]}')
    head += ['Settle: ' + s for s in settle[-1:]]
    head += ['Last second: ' + r.split(': ', 1)[-1] for r in ran[-1:]]
    head += ['Features on: ' + e[4:] for e in effects[-1:]]
    return lines_tile(head)


def sheet(tiles):
    """Labelled tiles, 3 to a row."""
    columns = min(3, len(tiles))
    rows = -(-len(tiles) // columns)
    label = 24
    out = Image.new('RGB', (columns * TILE[0], rows * (TILE[1] + label)), (12, 12, 14))
    draw = ImageDraw.Draw(out)
    for i, (name, tile) in enumerate(tiles):
        x, y = i % columns * TILE[0], i // columns * (TILE[1] + label)
        draw.text((x + 8, y + 3), name, font=font(16), fill=(255, 255, 255))
        out.paste(tile, (x, y + label))
    return out


def find_ffmpeg():
    known = Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs/Stremio/ffmpeg.exe'
    return str(known) if known.exists() else shutil.which('ffmpeg')


def remux(folder, out, lines):
    source = folder / 'Video.mjpeg'
    ffmpeg = find_ffmpeg()
    if not source.exists():
        return None
    if not ffmpeg:
        return 'Video.mjpeg left as it is: no ffmpeg found'
    video = tagged(lines, 'video')
    fps = re.search(r'\((\S+) fps\)', video[0]) if video else None
    # shortcut: one constant rate for the whole file; for real times use video_frame in Screen pixels.csv
    rate = fps.group(1) if fps and float(fps.group(1)) > 0 else '15'
    target = out / 'Video.avi'
    done = subprocess.run([ffmpeg, '-y', '-v', 'error', '-framerate', rate, '-f', 'mjpeg', '-i', str(source), '-c', 'copy', str(target)], capture_output=True, text=True)
    if done.returncode:
        return 'Video.avi failed: ' + done.stderr.strip()[:300]
    return f'{target} at {rate} fps'


def compare(folder, other, out):
    a, b = last_camera(folder), last_camera(other)
    if not a or not b:
        sys.exit(f'Refused: no [camera] line in {folder.name if not a else other.name}')
    why = camera_difference(a, b)
    if why:
        sys.exit(f'Refused: different cameras ({why})')
    shots = []
    for f in (folder, other):
        if not (f / 'Screenshot.png').exists():
            sys.exit(f'Refused: no Screenshot.png in {f.name}')
        shots.append(Image.open(f / 'Screenshot.png').convert('RGB'))
    if shots[0].size != shots[1].size:
        sys.exit('Refused: the screenshots have different sizes')
    difference = ImageChops.difference(*shots).point(lambda v: min(255, v * 4))
    tiles = [(folder.name, fit(lifted(shots[0]))), (other.name, fit(lifted(shots[1]))), ('Difference x4', fit(difference))]
    sheet(tiles).save(out / 'Comparison.png')
    return out / 'Comparison.png'


def pack(folder, out):
    lines = read_recording(folder)
    burst = read_burst(folder)
    tiles = []
    image = None
    shot = folder / 'Screenshot.png'
    if shot.exists():
        image = Image.open(shot).convert('RGB')
        bright = lifted(image)
        bright.save(out / 'Screenshot lifted.png')
        tiles += [('Screenshot', fit(image)), (f'Lifted, gamma {GAMMA}', fit(bright))]
    if (folder / 'AO alone.png').exists():
        tiles.append(('AO alone', fit(Image.open(folder / 'AO alone.png').convert('RGB'))))
    if (folder / 'Classes.png').exists():
        tiles.append(('Classes (cyan reads the cube: not the same as metal)', classes_tile(Image.open(folder / 'Classes.png').convert('RGB'), burst)))
    if (folder / 'White albedo.png').exists():
        tiles.append(('White albedo (experiment)', fit(Image.open(folder / 'White albedo.png').convert('RGB'))))
    probed = probed_tile(image, burst) if image else None
    if probed:
        tiles.append(('Light captures: the pixels', probed))
    tiles += [(name, fit(frame)) for name, frame in video_frames(folder)]
    cubes = strips_tile(sorted((folder / 'Cubes').glob('*.png'))) or cube_tile(tagged(lines, 'cube'))
    if cubes:
        tiles.append(('Light probes: last cube, faces +x -x +y -y +z -z, lifted', cubes))
    biggest = largest_map(folder)
    if biggest:
        tiles.append((f'Largest map: {biggest.name}', fit(lifted(Image.open(biggest)))))
    if burst:
        tiles.append(('The frames after the recording', burst_tile(folder, lines, burst)))
    if lines:
        tiles.append(('Recording', text_tile(lines, folder)))
    if not tiles:
        sys.exit(f'Nothing to pack in {folder}')
    sheet(tiles).save(out / 'Capture sheet.png')
    print(out / 'Capture sheet.png')
    if message := remux(folder, out, lines):
        print(message)


def main():
    parser = argparse.ArgumentParser(description='Makes a sheet, a lifted screenshot and a playable video of a recording folder.')
    parser.add_argument('folder', type=Path)
    parser.add_argument('--out', type=Path, help='where to write (default: the capture folder)')
    parser.add_argument('--compare', type=Path, help='another capture of the same view: only Comparison.png is written')
    args = parser.parse_args()
    out = args.out or args.folder
    out.mkdir(parents=True, exist_ok=True)
    if args.compare:
        print(compare(args.folder, args.compare, out))
    else:
        pack(args.folder, out)


if __name__ == '__main__':
    main()
