# nt31.mak - NMAKE makefile for Visual C++ 2.0 / 4.x (32-bit) targeting
# Windows NT 3.1+.  Uses the static CRT (LIBC.LIB) instead of ntcrt.c.
#   nmake /f build\nt31.mak
CC = cl
CFLAGS = /nologo /O2 /W3 /DWIN32 /D_WIN32 /DFLASHNT_NT31 /Isrc
LINK = link
LFLAGS = /nologo /subsystem:windows,3.10 /machine:ix86 /STACK:33554432
OBJS = util.obj reader.obj inflate.obj lzma.obj swf.obj shape.obj raster.obj render.obj stroke.obj \
       image.obj jpeg.obj player.obj avm1_str.obj avm1_value.obj avm1_obj.obj \
       avm1_interp.obj avm1_display.obj avm1_globals.obj avm1_builtins.obj avm1_mc.obj \
       avm1_movieclip.obj avm1_classes.obj avm1_date.obj avm1_xml.obj avm1_load.obj avm1_input.obj avm1_geom.obj avm1_filters.obj avm1_bitmap.obj avm1_amf.obj avm1_sound.obj avm1_snapshot.obj sock.obj avm1_fileref.obj avm1_netstream.obj bmd.obj fetch.obj avm1_text.obj avm1_playerglobal.obj casemap.obj dobj_geom.obj \
       text.obj morph.obj button.obj sound.obj \
       sound_codec.obj aac.obj jxr.obj jxr_sys.obj jxr_dec.obj mp3.obj video.obj render_filters.obj chars_free.obj abc.obj avm2_obj.obj avm2.obj avm2_lib.obj \
       avm2_regexp.obj avm2_verify.obj avm2_xml.obj avm2_describe.obj avm2_errmsg.obj avm2_amf.obj deflate.obj avm2_bytes.obj avm2_events.obj avm2_display.obj avm2_text.obj avm2_misc.obj avm2_geom3d.obj devfont.obj builtin_fonts.obj richtext.obj textfield.obj extval.obj numconv.obj avm2_builtin.obj filters.obj avm2_filters.obj avm2_bitmapdata.obj avm2_natives.obj avm2_fte.obj avm2_netstream.obj avm2_socket.obj avm2_fileref.obj avm2_localconn.obj avm2_netconn.obj avm2_extif.obj lc_hub.obj netstream.obj vdec.obj h263.obj vp6.obj h264.obj pbj.obj avm2_shader.obj avm2_sound.obj stage3d.obj avm2_stage3d.obj plat_win32.obj

RETROFP.EXE: $(OBJS) flashnt.res
	$(LINK) $(LFLAGS) /out:RETROFP.EXE $(OBJS) flashnt.res kernel32.lib user32.lib gdi32.lib winmm.lib comdlg32.lib wsock32.lib libc.lib

flashnt.res: res\flashnt.rc res\flashnt.ico
	rc /r /ires /fo flashnt.res res\flashnt.rc

.c.obj:
	$(CC) $(CFLAGS) /c src\$*.c

clean:
	del *.obj *.res RETROFP.EXE
