ISDB向けmpv
==========

## 追加・変更された機能

* デスクランブルしながらの再生
* DVBデバイスからの再生
  + ISDB-S/T用のチャンネル設定パラメータに対応
* 字幕の表示
  + "文字スーパー"他一部機能は未実装
  + 多少の位置ずれあり
  + 縦書き、複数音声対応(未テスト)
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

## 追加で必要となる依存ライブラリ

* [ISDB向けffmpeg](https://github.com/0p1pp1/ffmpeg) (必須)
* [ISDB向けlibass](https://github.com/0p1pp1/libass):
  + 字幕表示機能を使いたい場合に必要
* BCAS復号化ライブラリ:
  + デスクランブルしていないTSファイルの再生や、DVBデバイスからの再生をしたい場合に必要
  + libdemulti2: ベースのTSパケット復号ライブラリ +  
    そこから呼び出すECM復号化ライブラリ(いずれか一つ)
    - libpcsclite: PC/SCでBCASカードを使用する場合
    - libsobacas: ソフトウェアでのカードシミュレーション。 libpcsclite互換I/F
    - libyakisoba: ソフトウェアECM復号機能のみのライブラリ
  + libpcsclite以外の入手はt●r板のDTV関連@T●r/1-100やFNの_jp_2ch_dtvを探すか、
    ヘッダファイルを元に自作;)
* iconvモジュール[gconv-module-aribb25](https://github.com/0p1pp1/gconv-module-aribb25):
  + TS内のメタ情報を文字化けなく利用したい場合に必要  
    (現状ではチャンネル名がウインドウタイトルに表示されるのみ)

## ビルド方法

0. 外部依存ライブラリのインストール
    * BCAS復号化ライブラリ: libdemulti2 + {libpcsclite|libsobacas|libyakisoba}
        + ディストリ・パッケージ(libpcsclite)からか、自前でダウンロード・インストール
        + システムの標準パス(/usr/local/...)や`/etc/ld.so.conf[.d]`に指定されたパスにインストールするか、
          LD_LIBRARY_PATHにインストールパスを指定しておくこと
        + libsobacas/libyakisobaの場合は_bcas_keysも用意
    * iconvモジュール: gconv-module-aribb25
        + README.mdに従ってビルド・インストール(環境変数GCONV_PATHを設定しておくこと)

1. mpv と libass,ffmpegのビルド

    a). 自動(推奨)
```
./isdb-build
```
    (libassとffmpegをgithubからcloneしてconfigure、ビルドしmpvと**スタティック**リンクする。)

    b). 手動
    * libass,ffmpegのインストール 
```
./configure --prefix=/foo/mpv/build_libs [--enable-static [--disable-shared]] ...; make install
```
    * mpvをconfigure,ビルド
```
./bootstrap.py
export PKG_CONFIG_PATH=/foo/mpv/build_libs/lib/pkgconfig
./waf configure --enable-dvbin
./waf build
```
    * libass,ffmpegをシステム標準以外のパスにインストールした場合PKG_CONFIG_PATHの設定が必要
    * libass,ffmpegをstaticでリンクしたい場合、
    同じディレクトリ(上記の例だと`/foo/mpv/build_libs/lib`)か
    システム標準のパス(`/usr/lib`等)に同名の*.soが存在すると、
    そちらが優先してリンクされてしまうので注意。(./isdb-buildを参照)

## インストール・実行

`./waf install`で`/usr/local/bin`にインストールしてmpvで実行するか、  
インストールせずに直接`./build/mpv ...`で実行する。

libass,ffmpegをシステム標準のパス以外にインストールし、
mpvにdynamicリンクした場合は`/etc/ld.so.conf[.d]`かLD_LIBRARY_PATHの設定が必要となる。
libdemulti2他のライブラリについても同様。

TSのメタ情報を利用したい場合はGCONV_PATHの設定も必要。


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
* [Manual](http://mpv.io/manual/master/)


## Overview


**mpv** is a free (as in freedom) media player for the command line. It supports
a wide variety of media file formats, audio and video codecs, and subtitle types.

There is a [FAQ][FAQ].

Releases can be found on the [release list][releases].

## System requirements

- A not too ancient Linux, Windows 7 or later, or OSX 10.8 or later.
- A somewhat capable CPU. Hardware decoding might help if the CPU is too slow to
  decode video in realtime, but must be explicitly enabled with the `--hwdec`
  option.
- A not too crappy GPU. mpv's focus is not on power-efficient playback on
  embedded or integrated GPUs (for example, hardware decoding is not even
  enabled by default). Low power GPUs may cause issues like tearing, stutter,
  etc. The main video output uses shaders for video rendering and scaling,
  rather than GPU fixed function hardware. On Windows, you might want to make
  sure the graphics drivers are current. In some cases, ancient fallback video
  output methods can help (such as `--vo=xv` on Linux), but this use is not
  recommended or supported.


## Downloads


For semi-official builds and third-party packages please see
[mpv.io/installation](http://mpv.io/installation/).

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
external libraries. Below is a list of some important requirements.

The mpv build system uses [waf](https://waf.io/), but we don't store it in the
repository. The `./bootstrap.py` script will download the latest version
of waf that was tested with the build system.

For a list of the available build options use `./waf configure --help`. If
you think you have support for some feature installed but configure fails to
detect it, the file `build/config.log` may contain information about the
reasons for the failure.

NOTE: To avoid cluttering the output with unreadable spam, `--help` only shows
one of the two switches for each option. If the option is autodetected by
default, the `--disable-***` switch is printed; if the option is disabled by
default, the `--enable-***` switch is printed. Either way, you can use
`--enable-***` or `--disable-**` regardless of what is printed by `--help`.

To build the software you can use `./waf build`: the result of the compilation
will be located in `build/mpv`. You can use `./waf install` to install mpv
to the *prefix* after it is compiled.

Example:

    ./bootstrap.py
    ./waf configure
    ./waf
    ./waf install

Essential dependencies (incomplete list):

- gcc or clang
- X development headers (xlib, xrandr, xext, xscrnsaver, xinerama, libvdpau,
  libGL, GLX, EGL, xv, ...)
- Audio output development headers (libasound/ALSA, pulseaudio)
- FFmpeg libraries (libavutil libavcodec libavformat libswscale libavfilter
  and either libswresample or libavresample)
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
- harfbuzz (optional, required for correct rendering of combining characters,
  particularly for correct rendering of non-English text on OSX, and
  Arabic/Indic scripts on any platform)

FFmpeg dependencies (when building FFmpeg):

- gcc or clang, yasm on x86 and x86_64
- OpenSSL or GnuTLS (have to be explicitly enabled when compiling FFmpeg)
- libx264/libmp3lame/libfdk-aac if you want to use encoding (have to be
  explicitly enabled when compiling FFmpeg)
- For native DASH playback, FFmpeg needs to be built with --enable-libxml2
  (although there are security implications, and DAHS support has lots of bugs).
- AV1 decoding support requires dav1d.
- For good nvidia support on Linux, make sure nv-codec-headers is installed
  and can be found by configure.

Most of the above libraries are available in suitable versions on normal
Linux distributions. For ease of compiling the latest git master of everything,
you may wish to use the separately available build wrapper ([mpv-build][mpv-build])
which first compiles FFmpeg libraries and libass, and then compiles the player
statically linked against those.

If you want to build a Windows binary, you either have to use MSYS2 and MinGW,
or cross-compile from Linux with MinGW. See
[Windows compilation][windows_compilation].


## Release cycle

Every other month, an arbitrary git snapshot is made, and is assigned
a 0.X.0 version number. No further maintenance is done.

The goal of releases is to make Linux distributions happy. Linux distributions
are also expected to apply their own patches in case of bugs and security
issues.

Releases other than the latest release are unsupported and unmaintained.

See the [release policy document][release-policy] for more information.

## Bug reports


Please use the [issue tracker][issue-tracker] provided by GitHub to send us bug
reports or feature requests. Follow the template's instructions or the issue
will likely be ignored or closed as invalid.

Using the bug tracker as place for simple questions is fine but IRC is
recommended (see [Contact](#Contact) below).

## Contributing


Please read [contribute.md][contribute.md].

For small changes you can just send us pull requests through GitHub. For bigger
changes come and talk to us on IRC before you start working on them. It will
make code review easier for both parties later on.

You can check [the wiki](https://github.com/mpv-player/mpv/wiki/Stuff-to-do)
or the [issue tracker](https://github.com/mpv-player/mpv/issues?q=is%3Aopen+is%3Aissue+label%3A%22feature+request%22)
for ideas on what you could contribute with.

## License

GPLv2 "or later" by default, LGPLv2.1 "or later" with `--enable-lgpl`.
See [details.](https://github.com/mpv-player/mpv/blob/master/Copyright)

## History

This software is based on the MPlayer project. Before mpv existed as a project,
the code base was briefly developed under the mplayer2 project. For details,
see the [FAQ][FAQ].

## Contact


Most activity happens on the IRC channel and the github issue tracker.

- **GitHub issue tracker**: [issue tracker][issue-tracker] (report bugs here)
- **User IRC Channel**: `#mpv` on `irc.freenode.net`
- **Developer IRC Channel**: `#mpv-devel` on `irc.freenode.net`

[FAQ]: https://github.com/mpv-player/mpv/wiki/FAQ
[releases]: https://github.com/mpv-player/mpv/releases
[mpv-build]: https://github.com/mpv-player/mpv-build
[issue-tracker]:  https://github.com/mpv-player/mpv/issues
[release-policy]: https://github.com/mpv-player/mpv/blob/master/DOCS/release-policy.md
[windows_compilation]: https://github.com/mpv-player/mpv/blob/master/DOCS/compile-windows.md
[interface-changes]: https://github.com/mpv-player/mpv/blob/master/DOCS/interface-changes.rst
[api-changes]: https://github.com/mpv-player/mpv/blob/master/DOCS/client-api-changes.rst
[restore-old-bindings]: https://github.com/mpv-player/mpv/blob/master/etc/restore-old-bindings.conf
[contribute.md]: https://github.com/mpv-player/mpv/blob/master/DOCS/contribute.md
