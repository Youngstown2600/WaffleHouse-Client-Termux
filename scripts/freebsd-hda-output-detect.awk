# Detect conservative laptop-style snd_hda speaker/headphone layouts.
# Input: `sysctl -a` output.
# Output (TSV):
#   hdaa_unit speaker_nid target_as speaker_cur_as speaker_cur_seq
#   headphone_nid headphone_cur_as headphone_cur_seq
#   speaker_orig_as speaker_orig_seq headphone_orig_as headphone_orig_seq
#
# A candidate is emitted only when an hdaa function group exposes exactly one
# fixed Speaker pin and exactly one jack Headphones pin, with no Line-out pin.
# The association target comes from the firmware/original Speaker pin when
# available, otherwise from the current Speaker configuration.  This keeps the
# repair minimal: Speaker => target/seq0, Headphones => target/seq15.

function field_value(prefix,    i, p) {
    for (i = 2; i <= NF; i++) {
        if (index($i, prefix) == 1) {
            split($i, p, "=")
            return p[2]
        }
    }
    return ""
}

function key_parts(raw, parts,    n, leaf) {
    gsub(/:$/, "", raw)
    n = split(raw, parts, ".")
    if (n < 4 || parts[1] != "dev" || parts[2] != "hdaa")
        return 0
    leaf = parts[4]
    if (leaf !~ /^nid[0-9]+_(config|original)$/)
        return 0
    return 1
}

$1 ~ /^dev\.hdaa\.[0-9]+\.nid[0-9]+_(config|original):$/ {
    raw = $1
    if (!key_parts(raw, p))
        next

    unit = p[3]
    leaf = p[4]
    nid = leaf
    sub(/^nid/, "", nid)
    is_orig = (nid ~ /_original$/)
    sub(/_(config|original)$/, "", nid)

    asv = field_value("as=")
    seqv = field_value("seq=")
    devv = field_value("device=")
    connv = field_value("conn=")

    k = unit SUBSEP nid
    if (is_orig) {
        orig_as[k] = asv
        orig_seq[k] = seqv
        next
    }

    cur_as[k] = asv
    cur_seq[k] = seqv
    device[k] = devv
    conn[k] = connv

    if (devv == "Speaker" && connv == "Fixed") {
        spcount[unit]++
        spnid[unit] = nid
    } else if (devv == "Headphones" && connv == "Jack") {
        hpcount[unit]++
        hpnid[unit] = nid
    } else if (devv == "Line-out" && connv != "None") {
        linecount[unit]++
    }

    if ((devv == "Speaker" || devv == "Headphones" || devv == "Line-out") && connv != "None")
        analog_out_count[unit]++
}

END {
    for (unit in spcount) {
        if (spcount[unit] != 1 || hpcount[unit] != 1)
            continue
        if (linecount[unit] != 0 || analog_out_count[unit] != 2)
            continue

        sk = unit SUBSEP spnid[unit]
        hk = unit SUBSEP hpnid[unit]

        sas = cur_as[sk]
        ssq = cur_seq[sk]
        has = cur_as[hk]
        hsq = cur_seq[hk]
        osas = orig_as[sk]
        ossq = orig_seq[sk]
        ohas = orig_as[hk]
        ohsq = orig_seq[hk]

        target = osas
        if (target == "")
            target = sas

        # Association 0/15 and nonzero speaker sequences are too ambiguous for
        # an unattended rewrite.  Leave those machines advisory-only.
        if (target !~ /^[0-9]+$/ || target < 1 || target > 14)
            continue
        if (osas != "" && ossq != "" && ossq != 0)
            continue
        if (osas == "" && ssq != 0)
            continue

        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", \
            unit, spnid[unit], target, sas, ssq, hpnid[unit], has, hsq, \
            osas, ossq, ohas, ohsq
    }
}
