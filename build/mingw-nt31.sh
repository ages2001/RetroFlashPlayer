#!/bin/sh
# Cross-build RETROFP.EXE for Windows NT 3.1+ with i686-w64-mingw32-gcc.
# No CRT DLL is used (ntcrt.c), subsystem version is set to 3.10 so the
# NT 3.1 loader accepts the image, and code generation is restricted to
# plain i386/x87 instructions (no CMOV/SSE).
set -e
cd "$(dirname "$0")/.."
CC=${CC:-i686-w64-mingw32-gcc}
CFLAGS="-std=gnu89 -O2 -march=i386 -mtune=generic -mfpmath=387 -mno-sse -mno-mmx -fno-builtin -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident -Wall -Wno-misleading-indentation -DFLASHNT_NT31 -D_CRTIMP= -D__MINGW_IMPORT=extern -D__CRT_INLINE=static -D_CRTIMP_ALT= -D__MINGW_NOTHROW="
LDFLAGS="-nostdlib -nostartfiles -Wl,--subsystem,windows:3.10 -Wl,--major-os-version,3 -Wl,--minor-os-version,10 -Wl,-e,_WinMainCRTStartup -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--no-seh -Wl,--disable-reloc-section -Wl,--file-alignment,512 -Wl,--section-alignment,4096 -Wl,--stack,33554432 -Wl,-Map,build/obj/retrofp.map"
SRC="src/util.c src/reader.c src/inflate.c src/lzma.c src/swf.c src/shape.c src/raster.c src/raster_sse2.c src/render.c src/stroke.c src/image.c src/jpeg.c src/player.c src/avm1_str.c src/avm1_value.c src/avm1_obj.c src/avm1_interp.c src/avm1_display.c src/avm1_globals.c src/avm1_builtins.c src/avm1_mc.c src/avm1_movieclip.c src/avm1_classes.c src/avm1_date.c src/avm1_xml.c src/avm1_load.c src/avm1_input.c src/avm1_geom.c src/avm1_filters.c src/avm1_bitmap.c src/avm1_amf.c src/avm1_sound.c src/avm1_snapshot.c src/sock.c src/avm1_fileref.c src/avm1_netstream.c src/bmd.c src/fetch.c src/avm1_text.c src/avm1_playerglobal.c src/casemap.c src/dobj_geom.c src/text.c src/morph.c src/button.c src/sound.c src/sound_codec.c src/aac.c src/jxr.c src/jxr_sys.c src/jxr_dec.c src/mp3.c src/video.c src/render_filters.c src/chars_free.c src/abc.c src/avm2_obj.c src/avm2.c src/avm2_lib.c src/avm2_regexp.c src/avm2_verify.c src/avm2_xml.c src/avm2_describe.c src/avm2_errmsg.c src/avm2_amf.c src/deflate.c src/avm2_bytes.c src/avm2_events.c src/avm2_display.c src/avm2_text.c src/avm2_misc.c src/avm2_geom3d.c src/devfont.c src/builtin_fonts.c src/richtext.c src/textfield.c src/extval.c src/numconv.c src/avm2_builtin.c src/filters.c src/avm2_filters.c src/avm2_bitmapdata.c src/avm2_natives.c src/avm2_fte.c src/avm2_netstream.c src/avm2_socket.c src/avm2_fileref.c src/avm2_localconn.c src/avm2_netconn.c src/avm2_extif.c src/lc_hub.c src/netstream.c src/vdec.c src/h263.c src/vp6.c src/h264.c src/pbj.c src/avm2_shader.c src/avm2_sound.c src/stage3d.c src/avm2_stage3d.c src/ntcrt.c src/ntmath.c src/ntmath_sin.c src/ntmath_cos.c src/ntmath_tan.c src/ntmath_asin.c src/ntmath_acos.c src/ntmath_atan.c src/ntmath_atan2.c src/plat_win32.c"
mkdir -p dist build/obj
${WINDRES:-i686-w64-mingw32-windres} -i res/flashnt.rc -o build/obj/flashnt_res.o --include-dir res
$CC $CFLAGS $LDFLAGS -o dist/RETROFP.EXE $SRC build/obj/flashnt_res.o -lgcc -lkernel32 -luser32 -lgdi32 -lwinmm -lcomdlg32 -lwsock32
i686-w64-mingw32-strip dist/RETROFP.EXE 2>/dev/null || true
echo "built dist/RETROFP.EXE"
