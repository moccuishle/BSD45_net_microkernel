#!/usr/bin/env bash
# mkjlfs.sh -- build a minimal but valid 4.4BSD-Lite2 JLFS (UFS1) disk image.
#
# Pure Bash port of mkjlfs.py.  All binary data is produced with printf/xxd
# and placed with dd; python/python3 is NEVER invoked.
#
# Usage:
#   bash mkjlfs.sh -o disk.img [-size BYTES] [-ipg N] \
#        [--spec-file FILE] [dest=src ...]
#
#   -o/--out FILE     output image (required)
#   -size/--size N    image size in bytes (default 67108864)
#   -ipg N            inodes per cylinder group (default 128)
#   --spec-file FILE  text file with one 'dest=src' per line ('#' comments
#                     allowed).  Positional dest=src pairs are also accepted.
#
# On-disk format: 4.4BSD-Lite2 struct fs / struct dinode / struct direct,
# i386 little-endian layout.  See mkjlfs.py for the authoritative comments.

set -u

# ---------------------------------------------------------------- constants
DEV_BSIZE=512
BSIZE=4096
FSIZE=512
FRAG=$((BSIZE / FSIZE))          # 8
BSHIFT=12
FSHIFT=9
FRAGSHIFT=3
DEV_BSHIFT=9
FSBT_ODB=0

IFMT=$((0170000))
IFDIR=$((040000))
IFREG=$((0100000))

MAXNAMLEN=255
INOSIZE=128
INOPB=$((BSIZE / INOSIZE))       # 32
FS_MAGIC=$((0x011954))
NINDIR=$((BSIZE / 4))            # 1024

DT_UNKNOWN=0
DT_DIR=4
DT_REG=8

# ------------------------------------------------------------ hex utilities
# Reverse the byte order of an even-length hex string.
rev_hex_bytes() {
    local h="$1" out="" i
    for ((i = ${#h} - 2; i >= 0; i -= 2)); do
        out+="${h:i:2}"
    done
    printf '%s' "$out"
}

# Little-endian hex representation of integer $1 using $2 bytes.
le_hex() {
    local v="$1" n="$2" h
    h=$(printf "%0$((n * 2))x" "$v")
    rev_hex_bytes "$h"
}

# Set an n-byte little-endian integer at byte offset $2 in hex buffer named $1.
hex_set_le() {
    local -n _buf="$1"
    local off="$2" val="$3" n="${4:-4}"
    local hv pos=$((off * 2))
    hv=$(le_hex "$val" "$n")
    _buf="${_buf:0:$pos}${hv}${_buf:$((pos + ${#hv}))}"
}

# Set raw hex bytes at byte offset $2 in hex buffer named $1.
hex_set_raw() {
    local -n _buf="$1"
    local off="$2" raw="$3"
    local pos=$((off * 2))
    _buf="${_buf:0:$pos}${raw}${_buf:$((pos + ${#raw}))}"
}

# Write a hex buffer to the image at byte offset $2.
write_hex_buf() {
    local hex="$1" off="$2" img="$3"
    printf '%s' "$hex" | xxd -r -p |
        dd of="$img" bs=1 seek="$off" conv=notrunc status=none
}

# Read an n-byte little-endian integer from a hex string at byte offset.
hex_get_le() {
    local h="$1" off="$2" n="${3:-4}"
    local hb="${h:$((off * 2)):$((n * 2))}"
    printf '%d' "$((16#$(rev_hex_bytes "$hb")))"
}

# Set one bit in an in-memory hex buffer (named by $1).
hex_set_bit() {
    local -n _buf="$1"
    local byte="$2" bit="$3"
    local pos=$((byte * 2))
    local cur=$((16#${_buf:$pos:2}))
    _buf="${_buf:0:$pos}$(printf '%02x' $((cur | (1 << bit))))${_buf:$((pos + 2))}"
}

# ------------------------------------------------------------- arg parsing
OUT=""
SIZE=$((64 * 1024 * 1024))
IPG=128
SPEC_FILE=""
declare -a POSITIONAL=()

while (($#)); do
    case "$1" in
        -o|--out) OUT="$2"; shift 2 ;;
        -size|--size) SIZE="$2"; shift 2 ;;
        -ipg|--ipg) IPG="$2"; shift 2 ;;
        --spec-file) SPEC_FILE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,16p' "$0"
            exit 0
            ;;
        --) shift; POSITIONAL+=("$@"); break ;;
        -*) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done

if [[ -z "$OUT" ]]; then
    printf 'mkjlfs.sh: -o/--out is required\n' >&2
    exit 2
fi

# ------------------------------------------------- spec path normalization
normalize_dest() {
    local d="${1//\\//}"
    if [[ ! "$d" =~ ^[A-Za-z]:/ ]]; then
        printf '%s' "$d"
        return
    fi
    # Undo MSYS/Git-Bash path rewriting on a `/path=src` argument.
    local -a parts=() cleaned=()
    local p i j out
    IFS='/' read -ra parts <<< "$d"
    for p in "${parts[@]}"; do
        [[ -z "$p" || "$p" =~ ^[A-Za-z]:$ ]] && continue
        cleaned+=("$p")
    done
    local known=" bin sbin etc usr home tmp sys include lib var dev proc "
    for ((i = ${#cleaned[@]} - 1; i >= 0; i--)); do
        if [[ "$known" == *" ${cleaned[i]} "* ]]; then
            out="/"
            for ((j = i; j < ${#cleaned[@]}; j++)); do
                out+="${cleaned[j]}"
                ((j < ${#cleaned[@]} - 1)) && out+="/"
            done
            printf '%s' "$out"
            return
        fi
    done
    printf '/%s' "${cleaned[${#cleaned[@]} - 1]}"
}

expand_vars() {
    local s="$1"
    s="${s//\$\{HOME\}/$HOME}"
    s="${s//\$HOME/$HOME}"
    printf '%s' "$s"
}

# ------------------------------------------------------ node tree storage
declare -a NODE_PATH NODE_TYPE NODE_INO NODE_CHILDREN
declare -a NODE_SIZE NODE_FRAG NODE_DIRECT NODE_INDIRECT
declare -a NODE_INDIR_FRAG NODE_NBLOCKS NODE_SRC
NNODE=0

node_add() {
    NODE_PATH[$NNODE]="$1"
    NODE_TYPE[$NNODE]="$2"
    NODE_INO[$NNODE]="$3"
    NODE_CHILDREN[$NNODE]=""
    NODE_SIZE[$NNODE]=0
    NODE_FRAG[$NNODE]=0
    NODE_DIRECT[$NNODE]=""
    NODE_INDIRECT[$NNODE]=""
    NODE_INDIR_FRAG[$NNODE]=0
    NODE_NBLOCKS[$NNODE]=0
    NODE_SRC[$NNODE]=""
    NNODE=$((NNODE + 1))
}

node_index() {
    local p="$1" j
    for ((j = 0; j < NNODE; j++)); do
        if [[ "${NODE_PATH[j]}" == "$p" ]]; then
            printf '%s' "$j"
            return 0
        fi
    done
    printf '%s' -1
}

child_append() {
    local i="$1" ino="$2" dt="$3" name="$4"
    if [[ -n "${NODE_CHILDREN[i]}" ]]; then
        NODE_CHILDREN[i]+=";${ino},${dt},${name}"
    else
        NODE_CHILDREN[i]="${ino},${dt},${name}"
    fi
}

child_prepend() {
    local i="$1" ino="$2" dt="$3" name="$4"
    if [[ -n "${NODE_CHILDREN[i]}" ]]; then
        NODE_CHILDREN[i]="${ino},${dt},${name};${NODE_CHILDREN[i]}"
    else
        NODE_CHILDREN[i]="${ino},${dt},${name}"
    fi
}

# --------------------------------------------------------- geometry values
total_frags=$((SIZE / FSIZE))
ipg=$IPG
ncg=1
fpg=$total_frags
iblkno=32
inode_frags=$(( (ipg / INOPB) * FRAG ))
dblkno=$((iblkno + inode_frags))
csaddr=24
csaddr_frags=1

# --------------------------------------------------------- collect specs
declare -a SPECS=()
for a in "${POSITIONAL[@]}"; do
    SPECS+=("$a")
done
if [[ -n "$SPEC_FILE" ]]; then
    local_line=""
    while IFS= read -r local_line || [[ -n "$local_line" ]]; do
        # strip leading/trailing whitespace
        local_line="${local_line#"${local_line%%[![:space:]]*}"}"
        local_line="${local_line%"${local_line##*[![:space:]]}"}"
        [[ -z "$local_line" || "$local_line" == \#* ]] && continue
        SPECS+=("$local_line")
    done < "$SPEC_FILE"
fi

# ------------------------------------------------- parse file list & build
# Root directory: inode 2.
node_add "/" "d" 2
holder=3
next_frag=$dblkno

for spec in "${SPECS[@]}"; do
    if [[ "$spec" != *=* ]]; then
        printf 'bad spec (need dest=src): %s\n' "$spec" >&2
        exit 2
    fi
    dest="${spec%%=*}"
    src="${spec#*=}"
    dest=$(normalize_dest "$dest")
    src=$(expand_vars "$src")
    if [[ ! -f "$src" ]]; then
        printf 'missing source: %s\n' "$src" >&2
        exit 2
    fi

    [[ "$dest" == /* ]] || dest="/$dest"
    declare -a parts=()
    IFS='/' read -ra parts <<< "$dest"
    declare -a cleanparts=()
    for p in "${parts[@]}"; do
        [[ -n "$p" ]] && cleanparts+=("$p")
    done
    ((${#cleanparts[@]} == 0)) && continue

    parent="/"
    for ((pi = 0; pi < ${#cleanparts[@]} - 1; pi++)); do
        p="${cleanparts[pi]}"
        if [[ "$parent" == "/" ]]; then
            child="/$p"
        else
            child="$parent/$p"
        fi
        ci=$(node_index "$child")
        if ((ci < 0)); then
            pidx=$(node_index "$parent")
            node_add "$child" "d" "$holder"
            child_append "$pidx" "$holder" "$DT_DIR" "$p"
            holder=$((holder + 1))
        fi
        parent="$child"
    done

    name="${cleanparts[${#cleanparts[@]} - 1]}"
    ino=$holder
    holder=$((holder + 1))
    size=$(stat -c %s "$src")
    nblocks=$(( (size + BSIZE - 1) / BSIZE ))
    ((nblocks == 0)) && nblocks=1

    declare -a direct=()
    nd=$((nblocks < 12 ? nblocks : 12))
    for ((i = 0; i < nd; i++)); do
        direct+=("$next_frag")
        next_frag=$((next_frag + FRAG))
    done
    declare -a indirect=()
    indir_frag=0
    if ((nblocks > 12)); then
        indir_frag=$next_frag
        next_frag=$((next_frag + FRAG))
        for ((i = 12; i < nblocks; i++)); do
            indirect+=("$next_frag")
            next_frag=$((next_frag + FRAG))
        done
    fi

    ni=$NNODE
    node_add "$dest" "f" "$ino"
    NODE_SIZE[$ni]=$size
    NODE_DIRECT[$ni]="${direct[*]}"
    NODE_INDIRECT[$ni]="${indirect[*]}"
    NODE_INDIR_FRAG[$ni]=$indir_frag
    NODE_NBLOCKS[$ni]=$nblocks
    NODE_SRC[$ni]="$src"
    pidx=$(node_index "$parent")
    child_append "$pidx" "$ino" "$DT_REG" "$name"
done
next_ino=$holder

# ----------------------------------------- add '.' and '..' to directories
for ((i = 0; i < NNODE; i++)); do
    [[ "${NODE_TYPE[i]}" == "d" ]] || continue
    p="${NODE_PATH[i]}"
    if [[ "$p" != "/" ]]; then
        parent="${p%/*}"
        [[ -z "$parent" ]] && parent="/"
        pidx=$(node_index "$parent")
        child_prepend "$i" "${NODE_INO[$pidx]}" "$DT_DIR" ".."
    fi
    child_prepend "$i" "${NODE_INO[i]}" "$DT_DIR" "."
done

# ---------------------------------------------------------- one struct direct
dirent_hex() {
    local ino="$1" dt="$2" name="$3"
    local nl=${#name}
    local rl=$(( (8 + nl + 1 + 3) & ~3 ))
    local hdr nh pad padlen
    hdr="$(le_hex "$ino" 4)$(le_hex "$rl" 2)$(printf '%02x' "$dt")$(printf '%02x' "$nl")"
    nh=$(printf '%s' "$name" | xxd -p | tr -d '\n')
    padlen=$((rl - 8 - nl))
    pad=""
    if ((padlen > 0)); then
        pad=$(printf '00%.0s' $(seq 1 "$padlen"))
    fi
    printf '%s%s%s' "$hdr" "$nh" "$pad"
}

# -------------------------------------------------------------- create image
mkdir -p "$(dirname "$OUT")"
# Start from a guaranteed all-zero image.  truncate -s alone would leave
# stale bytes behind when the output file already exists (Python's
# bytearray(size) always starts zeroed).
: > "$OUT"
truncate -s "$SIZE" "$OUT"

# --------------------------------------------- write directory data blocks
for ((i = 0; i < NNODE; i++)); do
    [[ "${NODE_TYPE[i]}" == "d" ]] || continue
    blobhex=""
    declare -a chs=()
    IFS=';' read -ra chs <<< "${NODE_CHILDREN[i]}"
    for ch in "${chs[@]}"; do
        IFS=',' read -r c_ino c_dt c_name <<< "$ch"
        blobhex+="$(dirent_hex "$c_ino" "$c_dt" "$c_name")"
    done
    bloblen=$(( ${#blobhex} / 2 ))
    if ((bloblen > BSIZE)); then
        printf 'directory too big: %s\n' "${NODE_PATH[i]}" >&2
        exit 2
    fi
    frag=$next_frag
    next_frag=$((next_frag + FRAG))
    NODE_FRAG[$i]=$frag
    NODE_SIZE[$i]=$bloblen
    write_hex_buf "$blobhex" "$((frag * DEV_BSIZE))" "$OUT"
done

# ------------------------------------------- write indirect pointer blocks
for ((i = 0; i < NNODE; i++)); do
    [[ "${NODE_TYPE[i]}" == "f" ]] || continue
    ifr=${NODE_INDIR_FRAG[$i]}
    ((ifr == 0)) && continue
    declare -a ptrs=()
    read -ra ptrs <<< "${NODE_INDIRECT[$i]}"
    arrhex=""
    for ((j = 0; j < ${#ptrs[@]} && j < NINDIR; j++)); do
        arrhex+="$(le_hex "${ptrs[j]}" 4)"
    done
    need=$((BSIZE * 2 - ${#arrhex}))
    if ((need > 0)); then
        arrhex+="$(printf '0%.0s' $(seq 1 "$need"))"
    fi
    write_hex_buf "$arrhex" "$((ifr * DEV_BSIZE))" "$OUT"
done

# ------------------------------------------------------------ write file data
write_chunk() {
    local src="$1" bidx="$2" blk="$3" sz="$4"
    local off=$((bidx * BSIZE)) len=$BSIZE rem
    rem=$((sz - off))
    ((rem < 0)) && rem=0
    ((len > rem)) && len=$rem
    ((len <= 0)) && return 0
    dd if="$src" of="$OUT" bs="$len" \
        iflag=skip_bytes skip="$off" \
        oflag=seek_bytes seek="$((blk * DEV_BSIZE))" \
        count=1 conv=notrunc status=none
}

for ((i = 0; i < NNODE; i++)); do
    [[ "${NODE_TYPE[i]}" == "f" ]] || continue
    src="${NODE_SRC[i]}"
    sz=${NODE_SIZE[i]}
    declare -a direct=() indirect=()
    read -ra direct <<< "${NODE_DIRECT[i]}"
    read -ra indirect <<< "${NODE_INDIRECT[i]}"
    bidx=0
    for blk in "${direct[@]}"; do
        write_chunk "$src" "$bidx" "$blk" "$sz"
        bidx=$((bidx + 1))
    done
    for blk in "${indirect[@]}"; do
        write_chunk "$src" "$bidx" "$blk" "$sz"
        bidx=$((bidx + 1))
    done
done

# ------------------------------------------------------- build one struct dinode
build_dinode_hex() {
    local i="$1"
    local DIN
    DIN=$(printf '0%.0s' $(seq 1 256))    # 128 bytes = 256 hex chars
    local mode nlink size nblocks
    if [[ "${NODE_TYPE[i]}" == "d" ]]; then
        mode=$((040000 | 0755))
        nlink=2
        size=${NODE_SIZE[i]}
        if ((size > 0)); then nblocks=$FRAG; else nblocks=0; fi
    else
        mode=$((0100000 | 0644))
        nlink=1
        size=${NODE_SIZE[i]}
        nblocks=$((NODE_NBLOCKS[i] * FRAG))
    fi

    hex_set_le DIN 0 "$mode" 2       # di_mode
    hex_set_le DIN 2 "$nlink" 2      # di_nlink
    hex_set_le DIN 8 "$size" 8       # di_size (64-bit)

    # di_db[12]
    declare -a dbl=()
    if [[ "${NODE_TYPE[i]}" == "d" ]]; then
        dbl=("${NODE_FRAG[i]}")
    else
        read -ra dbl <<< "${NODE_DIRECT[i]}"
    fi
    for ((j = 0; j < 12; j++)); do
        v=0
        ((j < ${#dbl[@]})) && v=${dbl[j]}
        hex_set_le DIN $((40 + j * 4)) "$v" 4
    done

    # di_ib[0] holds the fragment number of the single-indirect block.
    v=0
    if [[ "${NODE_TYPE[i]}" != "d" ]] && (( ${NODE_INDIR_FRAG[i]} > 0 )); then
        v=${NODE_INDIR_FRAG[i]}
    fi
    hex_set_le DIN 88 "$v" 4

    hex_set_le DIN 104 "$nblocks" 4  # di_blocks
    hex_set_le DIN 108 1 4           # di_gen
    printf '%s' "$DIN"
}

# ----------------------------------------------------------- write inode table
for ((i = 0; i < NNODE; i++)); do
    ino=${NODE_INO[$i]}
    if ((ino <= 0 || ino > ipg)); then continue; fi
    off=$((iblkno * DEV_BSIZE + (ino - 1) * INOSIZE))
    write_hex_buf "$(build_dinode_hex "$i")" "$off" "$OUT"
done

# ------------------------------------------------------------- superblock
ndir=0
for ((i = 0; i < NNODE; i++)); do
    [[ "${NODE_TYPE[i]}" == "d" ]] && ndir=$((ndir + 1))
done
nifree=$((ipg - NNODE))

SB=$(printf '0%.0s' $(seq 1 16384))    # 8192 bytes
sblkno=$((8192 / DEV_BSIZE))            # 16
cblkno=$((sblkno + 8192 / BSIZE))       # 18

hex_set_le SB 0 0 4
hex_set_le SB 4 0 4
hex_set_le SB 8 "$sblkno" 4
hex_set_le SB 12 "$cblkno" 4
hex_set_le SB 16 "$iblkno" 4
hex_set_le SB 20 "$dblkno" 4
hex_set_le SB 24 0 4
hex_set_le SB 28 0 4
hex_set_le SB 32 0 4
hex_set_le SB 36 "$total_frags" 4
hex_set_le SB 40 "$total_frags" 4
hex_set_le SB 44 "$ncg" 4
hex_set_le SB 48 "$BSIZE" 4
hex_set_le SB 52 "$FSIZE" 4
hex_set_le SB 56 "$FRAG" 4
hex_set_le SB 60 0 4
hex_set_le SB 64 0 4
hex_set_le SB 68 0 4
hex_set_le SB 72 "$(( ~(BSIZE - 1) & 0xffffffff ))" 4
hex_set_le SB 76 "$(( ~(FSIZE - 1) & 0xffffffff ))" 4
hex_set_le SB 80 "$BSHIFT" 4
hex_set_le SB 84 "$FSHIFT" 4
hex_set_le SB 88 0 4
hex_set_le SB 92 0 4
hex_set_le SB 96 "$FRAGSHIFT" 4
hex_set_le SB 100 "$FSBT_ODB" 4
hex_set_le SB 104 8192 4
hex_set_le SB 108 0 4
hex_set_le SB 112 0 4
hex_set_le SB 116 "$NINDIR" 4
hex_set_le SB 120 "$INOPB" 4
hex_set_le SB 124 1 4
hex_set_le SB 128 0 4
hex_set_le SB 132 0 4
hex_set_le SB 136 0 4
hex_set_le SB 140 0 4
hex_set_le SB 144 0 4
hex_set_le SB 148 0 4
hex_set_le SB 152 "$csaddr" 4
hex_set_le SB 156 "$((csaddr_frags * DEV_BSIZE))" 4
hex_set_le SB 160 "$BSIZE" 4
hex_set_le SB 164 0 4
hex_set_le SB 168 0 4
hex_set_le SB 172 0 4
hex_set_le SB 176 0 4
hex_set_le SB 180 0 4
hex_set_le SB 184 "$ipg" 4
hex_set_le SB 188 "$fpg" 4
# fs_cstotal @192: csum_bytes(ndir, total_frags, nifree, total_frags)
hex_set_le SB 192 "$ndir" 4
hex_set_le SB 196 "$total_frags" 4
hex_set_le SB 200 "$nifree" 4
hex_set_le SB 204 "$total_frags" 4
hex_set_raw SB 209 "01"            # fs_clean = 1 (byte 208 fmod, 210 ronly, 211 flags = 0)
hex_set_raw SB 212 "2f"            # fs_fsmnt = "/"
hex_set_le SB 1324 2 4             # fs_inodefmt
hex_set_raw SB 1328 "ffffffffffffff7f"  # fs_maxfilesize = (1<<63)-1
hex_set_raw SB 1336 "00f0ffffffffffff"  # fs_qbmask = -4096
hex_set_raw SB 1344 "00feffffffffffff"  # fs_qfmask = -512
hex_set_le SB 1372 "$FS_MAGIC" 4    # fs_magic
write_hex_buf "$SB" 8192 "$OUT"

# --------------------------- cylinder-group summary: native JLFS bitmap -----
# mkjlfs.sh writes only a placeholder; this version emits the exact bitmap
# jlfs.c:cg_rebuild() would produce (scanning the dinodes already in OUT).
CG_MAGIC=$((0x4a4c4653))
CG_INOMAP=64
CG_FRAGMAP=128
IFMT=$((0170000))
CG=$(printf '0%.0s' $(seq 1 8192))    # 4096 bytes as hex
hex_set_le CG 0 "$CG_MAGIC" 4
for ((f = 0; f < dblkno; f++)); do
    hex_set_bit CG "$((CG_FRAGMAP + f / 8))" "$((f % 8))"
done
hex_set_bit CG "$CG_INOMAP" 0         # inode 1 reserved
for ((ino = 2; ino <= ipg; ino++)); do
    off=$((iblkno * DEV_BSIZE + (ino - 1) * INOSIZE))
    din=$(dd if="$OUT" bs=1 skip="$off" count="$INOSIZE" status=none | xxd -p -c "$INOSIZE" | tr -d '\n')
    mode=$(hex_get_le "$din" 0 2)
    (((mode & IFMT) == 0)) && continue
    hex_set_bit CG "$((CG_INOMAP + (ino - 1) / 8))" "$(((ino - 1) % 8))"
    for ((j = 0; j < 12; j++)); do
        frag=$(hex_get_le "$din" $((40 + j * 4)) 4)
        if ((frag >= dblkno && frag < total_frags)); then
            hex_set_bit CG "$((CG_FRAGMAP + frag / 8))" "$((frag % 8))"
        fi
    done
    indir=$(hex_get_le "$din" 88 4)
    if ((indir >= dblkno && indir < total_frags)); then
        hex_set_bit CG "$((CG_FRAGMAP + indir / 8))" "$((indir % 8))"
        ib=$(dd if="$OUT" bs=1 skip="$((indir * DEV_BSIZE))" count="$BSIZE" status=none | xxd -p -c "$BSIZE" | tr -d '\n')
        for ((j = 0; j < NINDIR; j++)); do
            frag=$(hex_get_le "$ib" $((j * 4)) 4)
            if ((frag >= dblkno && frag < total_frags)); then
                hex_set_bit CG "$((CG_FRAGMAP + frag / 8))" "$((frag % 8))"
            fi
        done
    fi
done
# write_hex_buf <hex> <byte offset> <image>: hex is the buffer, offset is where
# it goes.  (This call used to pass the image second, so dd got the path where
# it expected a number and the cylinder group - the block that carries the JLFS
# magic and the inode/frag bitmaps - was never written at all.)
write_hex_buf "$CG" "$((csaddr * DEV_BSIZE))" "$OUT"

# ------------------------------------------------------------------ checks
if ((next_frag > total_frags)); then
    printf 'out of fragments: used %d > %d; increase -size\n' \
        "$next_frag" "$total_frags" >&2
    exit 2
fi

printf 'JLFS image: %s (%d bytes)\n' "$OUT" "$SIZE"
printf '  superblock @8192  magic=0x%06x  bsize=%d fsize=%d\n' \
    "$FS_MAGIC" "$BSIZE" "$FSIZE"
printf '  inodes: %d/%d  inode_table@frag%d  data@frag%d\n' \
    "$NNODE" "$ipg" "$iblkno" "$dblkno"

exit 0
