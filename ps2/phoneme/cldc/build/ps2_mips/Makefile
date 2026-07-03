#
# Master Makefile for the ps2_mips build (phoneME Feature CLDC for PlayStation 2).
#
# This is a CROSS build: host tools (MakeDeps, jcc, preverify) and the romizer
# run on the x86 build host; the VM itself is cross-compiled for the Emotion
# Engine (MIPS r5900, little-endian) with the ps2sdk toolchain. Not self-hosted.
#
# See ps2_mips.cfg for the configuration and docker/phoneme-cross for the build
# environment. Authored for the j2me-ps2 port (mirrors linux_mips + javacall).
#
IsSelfHosted = false
include ../share/root.make
