# phoneME Feature — Stage A (host) build environment
#
# Purpose: reproducibly build the phoneME Feature *reference port* (javacall i386)
# to validate the two-stage pipeline — host tools + class-library romization —
# BEFORE retargeting the native VM to the PS2 (ps2sdk). No Cygwin; Linux host.
#
# Documented host requirement (phoneME Feature MR4): a JDK that can emit Java 1.4
# bytecode (historically JDK6) and a GCC able to emit 32-bit i686 code.
# JDK 8 still emits target 1.4 bytecode, so we base on Temurin 8 (maintained,
# working apt) and add gcc-multilib. Ancient build assumptions (Cygwin paths etc.)
# get patched incrementally against real build errors.

FROM eclipse-temurin:8-jdk

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc-multilib g++-multilib \
        make \
        ant \
        zip unzip \
        file procps \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# phoneME build scripts key off JDK_DIR. Temurin installs the JDK at JAVA_HOME.
ENV JDK_DIR=/opt/java/openjdk

# phoneME source is bind-mounted at run time (references/phoneme -> /phoneme),
# and the project workspace at /work, so nothing SDK-sized lives in the image.
WORKDIR /work

CMD ["bash"]
