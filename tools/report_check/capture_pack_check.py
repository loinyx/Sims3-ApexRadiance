#!/usr/bin/env python3
"""tools/capture_pack.py on a made-up recording folder: python tools/report_check/capture_pack_check.py <scratch folder>"""
import subprocess
import sys
import tempfile
from io import BytesIO
from pathlib import Path

from PIL import Image

TOOL = Path(__file__).resolve().parents[1] / 'capture_pack.py'
sys.path.insert(0, str(TOOL.parent))
import capture_pack  # noqa: E402

VP = ' '.join(['1.00000 0.00000 0.00000 3.00000', '0.00000 1.00000 0.00000 4.00000', '0.00000 0.00000 1.00001 -0.25000', '0.00000 0.00000 1.00000 5.00000'])
MIRRORED_VP = VP.replace('0.00000 1.00000 0.00000 4.00000', '0.00000 -1.00000 0.00000 9.00000')


def recording(folder, camera, shade=20):
    folder.mkdir(parents=True)
    frames = []
    for i in range(5):
        jpeg = BytesIO()
        Image.new('RGB', (64, 36), (i * 50, 10, 10)).save(jpeg, 'JPEG')
        frames.append(jpeg.getvalue())
    (folder / 'Video.mjpeg').write_bytes(b''.join(frames))
    Image.new('RGB', (64, 36), (shade, shade, shade)).save(folder / 'Screenshot.png')
    (folder / 'Recording.txt').write_text('\n'.join([
        'Apex Radiance recording: 10:00:00.000 to 10:00:10.000 (10.0 s), 5 lines',
        'Frame time over 100 samples: median 31.0 ms, p95 40.0 ms, longest 50.0 ms',
        '[video] Video.mjpeg: 64x36, 5 frames in 1.0 s (5.00 fps), every 2 frames, 0 dropped',
        '',
        '10:00:00.100 [effect] on: Ambient Occlusion, Night Lights',
        f'10:00:00.100 [camera] {camera}',
        '10:00:01.000 [ran] in the last 1.0 s: ambient occlusion 30, frames at the scene\'s end 30',
        '10:00:02.000 [cube] specular cube of probe 0x1234, 3 write(s), the last: 64 px | 10.0 20.0 30.0 | 1.0 2.0 3.0 | 4.0 5.0 6.0 | 7.0 8.0 9.0 | 0.0 0.0 0.0 | 255.0 255.0 255.0',
        '10:00:09.000 [settle] 8.0 s from the stop key to the save: 0 solve note(s) in that time',
        '',
        '==== ApexRadiance.toml at the start of the recording ====',
        '[camera] not a timeline line',
    ]), encoding='utf-8')
    return folder


def burst(folder):
    """What the frames after a recording leave in the folder."""
    Image.new('RGB', (64, 36), (128, 128, 128)).save(folder / 'AO alone.png')
    Image.new('RGB', (64, 36), (0, 255, 255)).save(folder / 'Classes.png')
    Image.new('RGB', (64, 36), (255, 255, 255)).save(folder / 'White albedo.png')
    for name in ('Cubes', 'Maps', 'Light capture mouse', 'Light capture cube'):
        (folder / name).mkdir()
    Image.new('RGB', (64 * 6, 64), (10, 20, 30)).save(folder / 'Cubes/specular 00001234.png')
    Image.new('RGB', (32, 16), (40, 10, 10)).save(folder / 'Maps/M1_32x16_A8R8G8B8.png')
    Image.new('L', (32, 16), 200).save(folder / 'Maps/M1_32x16_A8R8G8B8_alpha.png')
    for name in ('mouse', 'cube'):
        (folder / f'Light capture {name}/Light capture.txt').write_text(f'S3SS Light Probe | {name}\n== COR NA TELA NO PIXEL: (0.1 0.2 0.3)\n', encoding='utf-8')
    (folder / 'Lighting snapshot.txt').write_text('End: 3 lights, 1 lot stories, 2 rooms\n', encoding='utf-8')
    (folder / 'Burst.txt').write_text('\n'.join([
        '[view] AO alone.png: camera plain',
        '[class] reads the cube | 0 255 255 | 1600',
        '[class] other | 140 140 140 | 32',
        '[view] Classes.png: camera plain',
        '[probe] Light capture mouse: pixel 10 20, under the mouse',
        '[probe] Light capture cube: pixel 30 18, a pixel that reads the cube',
        '[map skipped] 5632x5632: larger than 2048 per side',
        '[view] White albedo.png: camera plain',
    ]), encoding='utf-8')
    with (folder / 'Recording.txt').open('a', encoding='utf-8') as f:
        f.write('\n')
    text = (folder / 'Recording.txt').read_text(encoding='utf-8')
    line = '10:00:03.000 [furniture] new rig 00000001 | box 0.50 x 1.00 x 0.50 m | lamp part\n'
    (folder / 'Recording.txt').write_text(text.replace('==== ApexRadiance.toml', line + '==== ApexRadiance.toml'), encoding='utf-8')


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True)


def main():
    assert capture_pack.lifted(Image.new('RGB', (1, 1), (0, 128, 255))).getpixel((0, 0)) == (0, round(255 * (128 / 255) ** 0.45), 255)
    assert len(capture_pack.split_jpegs(b'\xff\xd8\xffa\xff\xd9\xff\xd8\xffb\xff\xd9')) == 2
    with tempfile.TemporaryDirectory(dir=sys.argv[1] if len(sys.argv) > 1 else None) as scratch:
        root = Path(scratch)
        plain = f'plain near 0.2500 eye 1.000 2.000 3.000 vp {VP}'
        a = recording(root / 'a', plain)
        result = run(a, '--out', root / 'out')
        assert result.returncode == 0, result.stderr
        with Image.open(root / 'out/Capture sheet.png') as sheet:
            assert sheet.size == (3 * 960, 3 * 564), sheet.size  # screenshot, lifted, 3 video frames, cubes, text
        assert (root / 'out/Screenshot lifted.png').exists() and not (a / 'Capture sheet.png').exists()
        if capture_pack.find_ffmpeg():
            assert (root / 'out/Video.avi').stat().st_size > 0, result.stdout

        whole = recording(root / 'whole', plain)
        burst(whole)
        result = run(whole, '--out', root / 'whole out')
        assert result.returncode == 0, result.stderr
        with Image.open(root / 'whole out/Capture sheet.png') as sheet:
            assert sheet.size == (3 * 960, 5 * 564), sheet.size  # 9 above plus AO, classes, white, pixels, the map, the burst's text
        burst_lines = capture_pack.read_burst(whole)
        assert [t.split(':')[0] for t in capture_pack.tagged(burst_lines, 'probe')] == ['Light capture mouse', 'Light capture cube']
        text = ' '.join(capture_pack.capture_summary(whole / 'Light capture cube/Light capture.txt'))
        assert 'COR NA TELA' in text and capture_pack.largest_map(whole).name == 'M1_32x16_A8R8G8B8.png'
        assert capture_pack.classes_tile(Image.new('RGB', (64, 36)), burst_lines).getpixel((14, 14)) == (0, 255, 255)

        same = recording(root / 'same', f'mirrored near 0.2500 eye 1.000 2.000 3.000 vp {MIRRORED_VP}', 60)
        result = run(a, '--compare', same, '--out', root / 'cmp')
        assert result.returncode == 0, result.stderr  # the mirrored copy of the same camera is the same view
        assert (root / 'cmp/Comparison.png').exists()

        moved = recording(root / 'moved', f'plain near 0.2500 eye 1.500 2.000 3.000 vp {VP}')
        result = run(a, '--compare', moved, '--out', root / 'refused')
        assert result.returncode != 0 and 'different cameras' in result.stderr and not (root / 'refused/Comparison.png').exists(), result.stderr
        assert capture_pack.parse_camera(capture_pack.tagged(capture_pack.read_recording(a), 'camera')[-1])[2] == [1.0, 2.0, 3.0]

        turned = recording(root / 'turned', f'plain near 0.2500 eye 1.000 2.000 3.000 vp {VP.replace("0.00000 0.00000 1.00000 5.00000", "0.50000 0.00000 0.86603 5.00000")}')
        assert run(a, '--compare', turned, '--out', root / 'refused').returncode != 0

        empty = root / 'empty'
        empty.mkdir()
        assert run(empty).returncode != 0
    print('capture_pack: lift, sheet, video, comparison and refusals passed')


if __name__ == '__main__':
    main()
