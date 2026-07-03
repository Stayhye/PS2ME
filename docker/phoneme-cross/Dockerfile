# phoneME Feature — CLDC cross-build environment for the PlayStation 2 (ps2_mips).
#
# A single container needs BOTH toolchains, because the phoneME build is
# multi-pass and root.make runs them all in one `make`:
#   - HOST passes (tools/loopgen/romgen): Java (Temurin 8) + a 32-bit-capable GCC
#     (gcc-multilib), inherited from the phoneme-host image. romgen is built -m32
#     to model the 32-bit MIPS target's data layout.
#   - TARGET pass: the ps2sdk EE toolchain (mips64r5900el-ps2-elf-*), copied from
#     the rsdk-ps2-builder image.
#
# The EE toolchain is dynamically linked against musl, so we install the Debian
# `musl` package to provide /lib/ld-musl-x86_64.so.1; that lets the musl binaries
# run unchanged on this glibc base (the loader is self-contained, independent of
# glibc). This avoids a slow from-source toolchain build.
FROM phoneme-host

RUN apt-get update && apt-get install -y --no-install-recommends musl \
    && rm -rf /var/lib/apt/lists/*

# Prebuilt PS2 toolchain + ps2sdk from the EE builder image (~690 MB).
COPY --from=rsdk-ps2-builder:latest /usr/local/ps2dev /usr/local/ps2dev

# GCC's cc1/cc1plus (musl-linked) need musl builds of gmp/mpfr/mpc, which live in
# the Alpine image's /usr/lib. Copy them into an isolated dir and point ONLY the
# musl loader at it (via /etc/ld-musl-x86_64.path) so the glibc host gcc — which
# has its own gmp/mpfr/mpc under /usr/lib/x86_64-linux-gnu — is untouched.
COPY --from=rsdk-ps2-builder:latest /usr/lib/libgmp.so.10  /usr/local/ps2dev/musl-lib/libgmp.so.10
COPY --from=rsdk-ps2-builder:latest /usr/lib/libmpfr.so.6  /usr/local/ps2dev/musl-lib/libmpfr.so.6
COPY --from=rsdk-ps2-builder:latest /usr/lib/libmpc.so.3   /usr/local/ps2dev/musl-lib/libmpc.so.3
RUN printf '/usr/local/ps2dev/musl-lib\n/lib\n/usr/local/lib\n/usr/lib\n' > /etc/ld-musl-x86_64.path

# The phoneME build invokes the cross tools UNPREFIXED as $(GNU_TOOLS_DIR)/bin/{gcc,
# g++,as,ar}. ps2sdk ships them prefixed (mips64r5900el-ps2-elf-*). Provide wrapper
# scripts (not symlinks — a symlink named "gcc" could confuse the driver's target
# detection) that exec the prefixed tool. Point GNU_TOOLS_DIR at this dir at build.
RUN mkdir -p /usr/local/ps2dev/ee-crosswrap/bin && \
    for t in gcc g++ ar ld nm objcopy objdump ranlib strip; do \
      printf '#!/bin/sh\nexec mips64r5900el-ps2-elf-%s "$@"\n' "$t" \
        > /usr/local/ps2dev/ee-crosswrap/bin/$t && \
      chmod +x /usr/local/ps2dev/ee-crosswrap/bin/$t; \
    done && \
    printf '%s\n' \
      '#!/bin/sh' \
      '# phoneME hard-codes i386'"'"'s --32 into ASM_FLAGS (jvm.make). Drop i386-only' \
      '# assembler mode flags so the MIPS assembler does not choke on them.' \
      'args=""' \
      'for a in "$@"; do' \
      '  case "$a" in' \
      '    --32|--64) ;;' \
      '    *) args="$args $a" ;;' \
      '  esac' \
      'done' \
      'exec mips64r5900el-ps2-elf-as $args' \
      > /usr/local/ps2dev/ee-crosswrap/bin/as && \
    chmod +x /usr/local/ps2dev/ee-crosswrap/bin/as

ENV PS2DEV=/usr/local/ps2dev
ENV PS2SDK=/usr/local/ps2dev/ps2sdk
ENV PATH="/usr/local/ps2dev/bin:/usr/local/ps2dev/ee/bin:/usr/local/ps2dev/iop/bin:/usr/local/ps2dev/dvp/bin:${PATH}"

WORKDIR /work
CMD ["bash"]
