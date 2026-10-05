# Third-party notices

Moonlight PSP is distributed under the project's [GPL version 3 license](LICENSE). Retain the licenses and copyright notices included with the source and package.

| Component | Notice and source |
|---|---|
| intraFont 0.31 | **Uses intraFont by BenHur**. Uses parts of pgeFont by InsertWittyName. Creative Commons Attribution-Share Alike 3.0; see [license](docs/licenses/intraFont-LICENSE.txt), [original README](docs/licenses/intraFont-README.txt), and [source archive](https://github.com/PSP-Archive/intraFont). The installed SDK header identifies version 0.31. No intraFont library changes were made for this version. |
| Opus | Xiph.Org and the contributors listed in [COPYING](third_party/opus/COPYING). Keep that complete notice with binary distributions. |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later, as specified in the bundled [LICENSE](third_party/mbedtls/LICENSE). |
| OpenH264 (historical source) | Cisco Systems; BSD notice in [LICENSE](legacy/software/third_party/openh264/LICENSE). Preserved with earlier source under `legacy/software/`; excluded from the v1.5 application. |
| PSPSDK | PSP community authors; [BSD license](docs/licenses/PSPSDK-LICENSE.txt) and [source](https://github.com/pspdev/pspsdk). |
| libpng 1.4.4 | Glenn Randers-Pehrson and contributing authors; installed SDK notice retained in [libpng notice](docs/licenses/libpng-1.4.4-LICENSE.txt). |
| zlib 1.2.5 | Jean-loup Gailly and Mark Adler; installed SDK notice retained in [zlib notice](docs/licenses/zlib-1.2.5-LICENSE.txt). |
| Newlib 1.18.0 | Toolchain C runtime; [copyright and license notices](docs/licenses/newlib-1.18.0-COPYING.txt). |
| GNU C++ Library 4.3.5 | [Installed header notice and linking exception](docs/licenses/libstdc++-4.3.5-header-notice.txt), with [GPL version 2](docs/licenses/GNU-GPL-2.0.txt). |
| Controller prompt artwork | Nicolae (XELU) Berbece, Those Awesome Guys; CC0 [original prompts](https://thoseawesomeguys.com/prompts/). The five visible analog badges were authored for this project with `tools/generate_analog_badges.py`. |

PSP firmware fonts are loaded from `flash0:/font` at runtime. Sony PGF/BWFON firmware files are not included in the release package.

The source distribution includes the project source, assets, bundled dependencies, build recipes and notices.
