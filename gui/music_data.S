/*
 * Embedded background music. The two OGG Vorbis tracks are linked straight into
 * the binary with .incbin, so the GUI build stays a single self-contained file
 * (no external audio assets) on both Linux and the Windows cross-build. The
 * .ogg paths are resolved relative to the build's working directory (the repo
 * root), which is where the Makefile invokes the compiler.
 *
 * The matching `extern "C"` declarations live in gui/Main.cpp; track size is
 * the difference between the start and end labels.
 */
    .section .rodata

    .global sh_track1_ogg
    .global sh_track1_ogg_end
sh_track1_ogg:
    .incbin "gui/audio/track1.ogg"
sh_track1_ogg_end:

    .global sh_track2_ogg
    .global sh_track2_ogg_end
sh_track2_ogg:
    .incbin "gui/audio/track2.ogg"
sh_track2_ogg_end:

/* Mark the stack non-executable (ELF/Linux only); silences the linker warning
   and keeps the binary's stack NX. Not applicable to the Windows PE build. */
#if defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
