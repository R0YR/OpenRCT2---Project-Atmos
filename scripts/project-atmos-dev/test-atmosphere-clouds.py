#!/usr/bin/env python3
"""Compile the current cloud field with MSVC and check its rendering invariants.

Run from the checkout: python scripts/test-atmosphere-clouds.py
The generated executable and source go in artifacts/atmosphere-reference/cloud-checks.
Use --vcvars to select another Visual Studio installation.
"""

import argparse
from pathlib import Path
import subprocess


CHECKS = r"""
int main()
{
    CloudFrame c;
    c.size = 3000.0f;
    c.minWavelength = 30.0f;
    const float base[5] = { 0.52f, 0.27f, 0.14f, 0.07f, 0.03f };
    for (int i = 0; i < 5; i++)
        c.weight[i] = base[i] / 1.03f;

    assert(CloudShape(0.8f, 0.0f) == 0.0f);
    assert(CloudShape(0.1f, 0.5f) == 0.0f);
    assert(CloudShape(0.9f, 0.5f) == 1.0f);
    for (int n = 0; n <= 1000; n++)
    {
        float previous = 0.0f;
        for (int p = 0; p <= 100; p++)
        {
            const float shape = CloudShape(n / 1000.0f, p / 100.0f);
            assert(std::isfinite(shape) && shape >= previous && shape <= 1.0f);
            previous = shape;
        }
    }

    // A map-sized sample must contain genuine holes at partial coverage and become overcast at full coverage.
    for (float coverage : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        int empty = 0, dense = 0;
        float total = 0.0f;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++)
            {
                const float shape = CloudShape(CloudFbm(c, x * 40.0f, y * 40.0f, 1.0f, 0.0f), coverage);
                // Representative optical depth; this test reports field occupancy, not an in-game screenshot.
                const float alpha = 1.0f - std::exp(-shape * 3.9f * 0.75f);
                assert(std::isfinite(alpha) && alpha >= 0.0f && alpha <= 1.0f);
                empty += alpha == 0.0f;
                dense += alpha > 0.85f;
                total += alpha;
            }
        if (coverage == 0.0f)
            assert(empty == 65536 && total == 0.0f);
        if (coverage == 0.5f)
            assert(empty > 10000 && dense > 10000);
        if (coverage == 1.0f)
            assert(empty < 1000 && dense > 60000);
        printf("coverage %.2f: clear %.1f%%, dense %.1f%%, mean alpha %.3f\n",
            coverage, empty * 100.0f / 65536, dense * 100.0f / 65536, total / 65536);
    }

    // One-pixel steps across noise lattice and cloud boundaries must stay continuous at every supported game zoom.
    for (float wpp : { 1.0f, 2.0f, 4.0f, 8.0f })
    {
        c.minWavelength = 30.0f * wpp;
        float maxDelta = 0.0f;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 512; x++)
            {
                const float a = CloudShape(CloudFbm(c, x * wpp, y * 40.0f, 1.0f, 0.0f), 0.5f);
                const float b = CloudShape(CloudFbm(c, (x + 1) * wpp, y * 40.0f, 1.0f, 0.0f), 0.5f);
                maxDelta = std::max(maxDelta, std::abs(a - b));
            }
        assert(maxDelta < 0.03f);
        printf("wpp %.0f: max adjacent density delta %.5f\n", wpp, maxDelta);
    }
    puts("PASS finite support, monotone coverage, bounded opacity, zoom continuity");
}
"""


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vcvars",
        type=Path,
        default=Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"),
    )
    parser.add_argument("--output-dir", type=Path, default=root / "artifacts/atmosphere-reference/cloud-checks")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = args.output_dir.resolve()

    # Extract the implementation itself so the test never drifts into an independent noise implementation.
    source = (root / "src/openrct2-ui/drawing/Atmosphere.cpp").read_text()
    cloud_section = source.index("// Clouds. A drifting density field")
    start = source.index("        inline float SmoothStep01(", cloud_section)
    end = source.index("        // ---------------------------------------------------------------------------------------------------------",
                       source.index("        inline float CloudShape(", start))
    cpp = work / "cloud_checks.cpp"
    cpp.write_text(
        "#include <algorithm>\n#include <cmath>\n#include <cassert>\n#include <cstdint>\n"
        "#include <cstdio>\n#include <initializer_list>\n" + source[start:end] + CHECKS
    )
    command = (f'call "{args.vcvars}" >nul && cl /nologo /O2 /EHsc /std:c++20 "{cpp}" '
               f'/Fe:"{work / "cloud_checks.exe"}" /Fo:"{work / "cloud_checks.obj"}"')
    subprocess.run(command, shell=True, check=True)
    subprocess.run([str(work / "cloud_checks.exe")], check=True)


if __name__ == "__main__":
    main()
