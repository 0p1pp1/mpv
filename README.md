ISDB向けmpv
==========

## 追加・変更された機能

* デスクランブルしながらの再生
* DVBデバイスからの再生
  + ISDB-S/T用のチャンネル設定パラメータに対応
* デュアルモノ音声対応
  + 主音声のデフォルト再生
  + 言語指定による自動選択
  + ユーザによる選択・切り替え
* TSストリーム内での番組切り替わり(次番組開始)対応
  + 番組の切り替わり時点で終了していたのを再生継続するように
* 複数プログラム、複数トラック(映像・音声・字幕)が存在する際のトラック選択の改良
  + 番組(SID)の指定オプション
  + デフォルトフラグ、言語、所属プログラムを考慮したトラック自動選択

なお本修正はLinuxにのみ対応する。
### 本ブランチ`isdb-0.38`での主な変更点
* 本家mpvの`master`ブランチのv0.38.0タグにリベース
* 字幕表示機能は、本家側がffmpeg/libaribcaptionに対応したので、そちらを利用することとし、
  従来からの(ASS拡張による)独自実装は削除した。
* 上記変更に伴い、パッチが必要な依存ライブラリもISDB向けffmpegだけとなった
* (ISDB向けパッチも最新版にリベース. ffmpeg:7.0)

## 追加で必要となる依存ライブラリ

* [ISDB向けffmpeg](https://github.com/0p1pp1/ffmpeg) (ほぼ必須)
* BCAS復号化ライブラリ:
  + デスクランブルしていないTSファイルの再生や、DVBデバイスからの再生をしたい場合に必要
  + libdemulti2: ベースのTSパケット復号ライブラリ +  
    そこから呼び出すECM復号化ライブラリ(いずれか一つ)
    - libpcsclite: PC/SCでBCASカードを使用する場合
    - libsobacas: ソフトウェアでのカードシミュレーション。 libpcsclite互換I/F
    - libyakisoba: ソフトウェアECM復号機能のみのライブラリ
  + libpcsclite以外の入手はt●r板のDTV関連@T●r/1-100やFNの_jp_2ch_dtvを探すか、
    ヘッダファイルを元に自作;)
* iconvモジュール[gconv-module-aribb24](https://github.com/0p1pp1/gconv-module-aribb24):
  + TS内のメタ情報を文字化けなく利用したい場合に必要  
    (現状ではチャンネル名がウインドウタイトルに表示されるのみ)

## ビルド方法

0. BCAS復号化ライブラリのビルド・インストール(オプション)

libdemulti2に加え、{libpcsclite|libsobacas|libyakisoba}のいずれかをインストール

1. ISDB用iconvモジュールのビルド・インストール(オプション)
```
cd <somewhere>
git clone [--depth 1] https://github.com/0p1pp1/gconv-module-aribb24
cd gconv-module-aribb24 ; ./autogen.sh
mkdir build; cd build
../configure
make
sudo make install
echo 'export GCONV_PATH=/usr/local/lib/gconv/aribb24' | \
    sudo tee /etc/profile.d/gconv-module-aribb24.sh
```
2. libaribcaptionのビルド・インストール

[本家](https://github.com/xqq/libaribcaption) 参照。(字幕表示機能が必要な場合のみ)

3. ISDB向けffmpegのビルド・インストール

一応オプションであるが、ほぼ必要。下記の例のようにffmpegが外部ライブラリでサポートしている
コーデック/機能を使用したい場合は、事前に該当ライブラリをインストールしておく必要あり。

```
cd <somewhere>
git clone [--depth 1] -b isdb-7.0 https://github.com/0p1pp1/ffmpeg
cd ffmpeg
mkdir build; cd build
# 上記2.でlibaribcaptionを/usr/local/libにインストールしたなら
# export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
../configure --disable-doc --enable-gpl --enable-nonfree --enable-libass \
  --enable-gnutls --enable-libbluray --enable-libaom --enable-libdav1d \
  --enable-libmp3lame --enable-libtwolame --enable-libpulse \
  --enable-libtheora --enable-libopus --enable-libvorbis --enable-libvpx \
  --enable-libx264 --enable-libx265 --enable-libiec61883 --enable-libxml2  \
  --enable-opengl --enable-libdrm --enable-libaribcaption # --enable-libdemulti2
make
sudo make install
# すでに実行していなければ...
# echo '/usr/local/lib' | sudo tee /etc/ld.so.conf.d/local.conf
sudo ldconfig
```

4. mpvのビルド
```
cd <somewhere>
git clone [--depth 1] -b isdb-0.38 https://github.com/0p1pp1/mpv
cd mpv
meson setup --pkg-config-path /usr/local/lib/pkgconfig -D dvbin=enabled build
meson compile -C build
```

## インストール・実行

`meson install -C build`で`/usr/local/bin`にインストールしてmpvで実行するか、  
インストールせずに直接`./build/mpv ...`で実行する。

### ISDB独自機能関連のUI
- コマンドラインでのDVBチャンネル指定: `mpv dvb://[カード番号@]チャンネル名`
    * チャンネル設定ファイル`$XDG_CONFIG_HOME/mpv/channels.conf`が必要。
        + [ISDB向けmplayer](https://github.com/0p1pp1/mplayer)と同じ1行1チャンネルの形式
        + 地デジ向けはchannels.conf.isdbt, 衛星向けはchannels.conf.isdbsに分けて指定も可能
        + 上記デフォルト名以外のチャンネル設定ファイルは`--dvbin-file=`オプションで指定可
        + [dvbアプリ集](https://github.com/0p1pp1/dvb_apps)の`s2scan`コマンドで自動生成可能
    * `dvb//1@NHK?frontend=1&demux=1&dvr=1`のようなオプション指定も一応可能
    * デフォルトの設定だと再生開始まで時間がかかるので、
      `--demuxer-lavf-probesize=2000000`のようにオプションを指定するか、
      `$XDG_CONFIG_HOME/mpv/mpv.conf`にDVBの自動プロファイルとして指定すると良い。
```
hwdec=auto

[protocol.dvb]
profile-desc="profile for dvb:// streams"
demuxer-lavf-probesize=2000000
demuxer-lavf-analyzeduration=1.3
```
- 字幕のオン・オフ/切り替え: 'j'キー(`cycle sub`コマンド)
    * 複数言語字幕の場合も'j'キーでオフ->日本語字幕->英語字幕->オフのように切り替わる（はず）
- `--slang=jpn`コマンドラインオプションで、日本語字幕が有る時は自動的に表示
- デュアルモノラル音声の場合の主・副・両方の指定/切り替え：
    * `--dmono={auto|main|sub|both}`コマンドラインオプションで指定
    * `dmono-mode`プロパティへの設定で切り替え。`input.conf`に`A cycle dmono-mode`等
    * 一般の音声切り替えコマンド(`cycle audio`,'#'キー)でも主->副->次の音声トラックに切り替え
- 再生するプログラムIDの明示指定:
    * `--progid=<SID>`コマンドラインオプション
    * `program`プロパティへの設定。(従来と同じ。 `input.conf`等で設定・切り替え)

キーバインディング/利用可能コマンドについては`etc/input.conf`やmpv(1)を参照。
`$XDG_CONFIG_HOME/mpv/input.conf`でカスタマイズできる。

以下、オリジナルのREADME.md

----

![mpv logo](https://raw.githubusercontent.com/mpv-player/mpv.io/master/source/images/mpv-logo-128.png)

# mpv


* [External links](#external-links)
* [Overview](#overview)
* [System requirements](#system-requirements)
* [Downloads](#downloads)
* [Changelog](#changelog)
* [Compilation](#compilation)
* [Release cycle](#release-cycle)
* [Bug reports](#bug-reports)
* [Contributing](#contributing)
* [License](#license)
* [Contact](#contact)


## External links


* [Wiki](https://github.com/mpv-player/mpv/wiki)
* [FAQ][FAQ]
* [Manual](https://mpv.io/manual/master/)


## Overview


**mpv** is a free (as in freedom) media player for the command line. It supports
a wide variety of media file formats, audio and video codecs, and subtitle types.

There is a [FAQ][FAQ].

Releases can be found on the [release list][releases].

## System requirements

- A not too ancient Linux (usually, only the latest releases of distributions
  are actively supported), Windows 10 or later, or macOS 10.15 or later.
- A somewhat capable CPU. Hardware decoding might help if the CPU is too slow to
  decode video in realtime, but must be explicitly enabled with the `--hwdec`
  option.
- A not too crappy GPU. mpv's focus is not on power-efficient playback on
  embedded or integrated GPUs (for example, hardware decoding is not even
  enabled by default). Low power GPUs may cause issues like tearing, stutter,
  etc. On such GPUs, it's recommended to use `--profile=fast` for smooth playback.
  The main video output uses shaders for video rendering and scaling,
  rather than GPU fixed function hardware. On Windows, you might want to make
  sure the graphics drivers are current. In some cases, ancient fallback video
  output methods can help (such as `--vo=xv` on Linux), but this use is not
  recommended or supported.

mpv does not go out of its way to break on older hardware or old, unsupported
operating systems, but development is not done with them in mind. Keeping
compatibility with such setups is not guaranteed. If things work, consider it
a happy accident.

## Downloads


For semi-official builds and third-party packages please see
[mpv.io/installation](https://mpv.io/installation/).

## Changelog


There is no complete changelog; however, changes to the player core interface
are listed in the [interface changelog][interface-changes].

Changes to the C API are documented in the [client API changelog][api-changes].

The [release list][releases] has a summary of most of the important changes
on every release.

Changes to the default key bindings are indicated in
[restore-old-bindings.conf][restore-old-bindings].

## Compilation


Compiling with full features requires development files for several
external libraries. Mpv requires [meson](https://mesonbuild.com/index.html)
to build. Meson can be obtained from your distro or PyPI.

After creating your build directory (e.g. `meson setup build`), you can view a list
of all the build options via `meson configure build`. You could also just simply
look at the `meson_options.txt` file. Logs are stored in `meson-logs` within
your build directory.

Example:

    meson setup build
    meson compile -C build
    meson install -C build

For libplacebo, meson can use a git check out as a subproject for a convenient
way to compile mpv if a sufficient libplacebo version is not easily available
in the build environment. It will be statically linked with mpv. Example:

    mkdir -p subprojects
    git clone https://code.videolan.org/videolan/libplacebo.git --depth=1 --recursive subprojects/libplacebo

Essential dependencies (incomplete list):

- gcc or clang
- X development headers (xlib, xrandr, xext, xscrnsaver, xpresent, libvdpau,
  libGL, GLX, EGL, xv, ...)
- Audio output development headers (libasound/ALSA, pulseaudio)
- FFmpeg libraries (libavutil libavcodec libavformat libswscale libavfilter
  and either libswresample or libavresample)
- libplacebo
- zlib
- iconv (normally provided by the system libc)
- libass (OSD, OSC, text subtitles)
- Lua (optional, required for the OSC pseudo-GUI and youtube-dl integration)
- libjpeg (optional, used for screenshots only)
- uchardet (optional, for subtitle charset detection)
- nvdec and vaapi libraries for hardware decoding on Linux (optional)

Libass dependencies (when building libass):

- gcc or clang, yasm on x86 and x86_64
- fribidi, freetype, fontconfig development headers (for libass)
- harfbuzz (required for correct rendering of combining characters, particularly
  for correct rendering of non-English text on macOS, and Arabic/Indic scripts on
  any platform)

FFmpeg dependencies (when building FFmpeg):

- gcc or clang, yasm on x86 and x86_64
- OpenSSL or GnuTLS (have to be explicitly enabled when compiling FFmpeg)
- libx264/libmp3lame/libfdk-aac if you want to use encoding (have to be
  explicitly enabled when compiling FFmpeg)
- For native DASH playback, FFmpeg needs to be built with --enable-libxml2
  (although there are security implications, and DASH support has lots of bugs).
- AV1 decoding support requires dav1d.
- For good nvidia support on Linux, make sure nv-codec-headers is installed
  and can be found by configure.

Most of the above libraries are available in suitable versions on normal
Linux distributions. For ease of compiling the latest git master of everything,
you may wish to use the separately available build wrapper ([mpv-build][mpv-build])
which first compiles FFmpeg libraries and libass, and then compiles the player
statically linked against those.

If you want to build a Windows binary, see [Windows compilation][windows_compilation].


## Release cycle

Once or twice a year, a release is cut off from the current development state
and is assigned a 0.X.0 version number. No further maintenance is done, except
in the event of security issues.

The goal of releases is to make Linux distributions happy. Linux distributions
are also expected to apply their own patches in case of bugs.

Releases other than the latest release are unsupported and unmaintained.

See the [release policy document][release-policy] for more information.

## Bug reports


Please use the [issue tracker][issue-tracker] provided by GitHub to send us bug
reports or feature requests. Follow the template's instructions or the issue
will likely be ignored or closed as invalid.

Questions can be asked in the [discussions][discussions] or on IRC (see
[Contact](#Contact) below).

## Contributing


Please read [contribute.md][contribute.md].

For small changes you can just send us pull requests through GitHub. For bigger
changes come and talk to us on IRC before you start working on them. It will
make code review easier for both parties later on.

You can check [the wiki](https://github.com/mpv-player/mpv/wiki/Stuff-to-do)
or the [issue tracker](https://github.com/mpv-player/mpv/issues?q=is%3Aopen+is%3Aissue+label%3Ameta%3Afeature-request)
for ideas on what you could contribute with.

## License

GPLv2 "or later" by default, LGPLv2.1 "or later" with `-Dgpl=false`.
See [details.](https://github.com/mpv-player/mpv/blob/master/Copyright)

## History

This software is based on the MPlayer project. Before mpv existed as a project,
the code base was briefly developed under the mplayer2 project. For details,
see the [FAQ][FAQ].

## Contact


Most activity happens on the IRC channel and the GitHub issue tracker.

- **GitHub issue tracker**: [issue tracker][issue-tracker] (report bugs here)
- **Discussions**: [discussions][discussions]
- **User IRC Channel**: `#mpv` on `irc.libera.chat`
- **Developer IRC Channel**: `#mpv-devel` on `irc.libera.chat`

[FAQ]: https://github.com/mpv-player/mpv/wiki/FAQ
[releases]: https://github.com/mpv-player/mpv/releases
[mpv-build]: https://github.com/mpv-player/mpv-build
[issue-tracker]:  https://github.com/mpv-player/mpv/issues
[discussions]: https://github.com/mpv-player/mpv/discussions
[release-policy]: https://github.com/mpv-player/mpv/blob/master/DOCS/release-policy.md
[windows_compilation]: https://github.com/mpv-player/mpv/blob/master/DOCS/compile-windows.md
[interface-changes]: https://github.com/mpv-player/mpv/blob/master/DOCS/interface-changes.rst
[api-changes]: https://github.com/mpv-player/mpv/blob/master/DOCS/client-api-changes.rst
[restore-old-bindings]: https://github.com/mpv-player/mpv/blob/master/etc/restore-old-bindings.conf
[contribute.md]: https://github.com/mpv-player/mpv/blob/master/DOCS/contribute.md
