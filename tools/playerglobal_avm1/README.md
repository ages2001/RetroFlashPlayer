# AVM1 playerglobals written in ActionScript

Flash Player implements part of its AVM1 global library (AsBroadcaster,
Error, ContextMenu, flash.geom.Point/Rectangle/Matrix, ExternalInterface,
`trace`) in ActionScript inside its own playerglobals.  The `.as` files in
this directory reproduce that code.  They come from the Ruffle project
(https://github.com/ruffle-rs/ruffle, MIT / Apache-2.0) and are used
unchanged, except for the "class" case of ExternalInterface._toAS (it
evaluates the class name, as Flash does).

FLASHNT ships the compiled bytecode in `src/avm1_playerglobal.c`; the
player runs it once for each global environment after the native classes
are created.  To regenerate it:

    cd tools/playerglobal_avm1/compiler
    cargo run --release -- .. ../../../build/playerglobal_avm1.swf
    cd ../../..
    python3 tools/gen_playerglobal.py build/playerglobal_avm1.swf > src/avm1_playerglobal.c

The compiler is `rascal`, an ActionScript 2 compiler written in Rust.
