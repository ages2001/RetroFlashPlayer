# FLASHNT host build (gcc/clang, any POSIX).  For the NT 3.1 binary see
# build/nt31.mak (MSVC 2.0/4.x) and build/mingw-nt31.sh.
CC      ?= gcc
CFLAGS  ?= -std=c89 -O2 -Wall -Wno-misleading-indentation
LDFLAGS ?= -lm

CORE = src/util.c src/reader.c src/inflate.c src/lzma.c src/swf.c src/shape.c \
       src/raster.c src/render.c src/stroke.c src/image.c src/player.c \
       src/text.c src/morph.c src/button.c src/sound.c src/sound_codec.c src/aac.c src/jxr.c src/jxr_sys.c src/jxr_dec.c src/mp3.c src/video.c src/render_filters.c src/jpeg.c \
       src/avm1_str.c src/avm1_value.c src/avm1_obj.c src/avm1_interp.c src/avm1_display.c src/avm1_globals.c \
       src/avm1_builtins.c src/avm1_mc.c src/avm1_movieclip.c src/avm1_classes.c src/avm1_date.c src/avm1_xml.c src/avm1_load.c src/avm1_input.c src/avm1_geom.c src/avm1_filters.c src/avm1_bitmap.c src/avm1_amf.c src/avm1_sound.c src/avm1_snapshot.c src/sock.c src/avm1_fileref.c src/avm1_netstream.c src/bmd.c src/fetch.c src/avm1_text.c src/avm1_playerglobal.c \
       src/casemap.c src/dobj_geom.c src/chars_free.c \
       src/abc.c src/avm2_obj.c src/avm2.c src/avm2_lib.c src/avm2_regexp.c src/avm2_verify.c src/avm2_xml.c src/avm2_describe.c src/avm2_errmsg.c src/avm2_amf.c src/deflate.c src/avm2_bytes.c \
       src/avm2_events.c src/avm2_display.c src/avm2_text.c src/avm2_misc.c src/avm2_geom3d.c src/devfont.c src/builtin_fonts.c src/richtext.c src/textfield.c src/extval.c src/numconv.c src/avm2_builtin.c src/filters.c src/avm2_filters.c src/avm2_bitmapdata.c src/avm2_natives.c src/avm2_fte.c src/avm2_netstream.c src/avm2_socket.c src/avm2_fileref.c src/avm2_localconn.c src/avm2_netconn.c src/avm2_extif.c src/lc_hub.c src/netstream.c src/vdec.c src/h263.c src/vp6.c src/h264.c src/pbj.c src/avm2_shader.c src/avm2_sound.c src/stage3d.c src/avm2_stage3d.c

all: flashnt-host

flashnt-host: $(CORE) src/plat_host.c src/testfont.c src/testfont_r.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f flashnt-host *.o

.PHONY: all clean
