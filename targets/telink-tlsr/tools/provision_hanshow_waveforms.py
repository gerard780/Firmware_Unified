#!/usr/bin/env python3
"""Generate a private, hash-verified native waveform provider; no hardware access.

Manifest: {"profiles": [{"panel": "154", "band": 6, "temperature_c": 24.5,
"records": ["pass0.bin", "pass1.bin"]}]}. For 266, optional "waveform_family"
selects "0cnspq1tl" (default, band 6) or "0enspq1yq" (band 5).
A profile may instead name "backup"
with an exact 512 KiB factory capture. Paths resolve against the manifest.
Outputs must stay outside this repository. Redistribution rights are unresolved.
"""

import argparse
from decimal import Decimal, InvalidOperation
import hashlib
import json
from pathlib import Path
import os

WAVE_BYTES = 535
# Fingerprints and extension bytes are captured hardware facts, not waveform data.
PROFILES = {
    ("266", "0cnspq1tl", 6): (1036, 2400, 2500, "090006070f0a2f25222e21", (
        "e81204bcc8dd464ebc54ccade6c2d945e77c89a14d9d02e1beb3e7ddda4d0752",
        "85c4a1401b21a098f563040a044d06feb61925b32f92d8d21ea14e0cd435aff4")),
    ("266", "0enspq1yq", 5): (1036, 2001, 2500, "130006070f0a2f25222e2160020202650400000000", (
        "9d8fa882b31f725834142184caa401d0b6768747b69273741be312e29c5293a0",
        "66408af760070bb0e6ceac45d623fd69752b59b72e5409d08af28b228a9f66da")),
    ("154", "154", 6): (1035, 2400, 2500, "090006070f0a2f25222b21", (
        "786473323a0847417cc04036b37faa107b5e2e7d824cffd52c126d6f9532f5a1",
        "464513739a13052927f740e37be08321a65c3bd6eee261224a4ed0208e52f76b")),
    ("154", "154", 7): (1035, 2501, 3000, "090006070f0a2f25222b21", (
        "02612d53455a5c2707dd0cf3cf9c935bac9f42ef433dae5a2cda86a789596a2f",
        "464513739a13052927f740e37be08321a65c3bd6eee261224a4ed0208e52f76b")),
}
REPOSITORY = Path(__file__).resolve().parents[3]


def load_profiles(manifest_path):
    """Validate every input before opening an output; never guess panel or band."""
    manifest_path = Path(manifest_path)
    document = json.loads(manifest_path.read_text())
    if not isinstance(document, dict):
        raise ValueError("manifest must be an object")
    specs = document.get("profiles")
    if not isinstance(specs, list) or not specs:
        raise ValueError("manifest needs a nonempty profiles list")
    result = []
    seen = set()
    for spec in specs:
        if not isinstance(spec, dict):
            raise ValueError("each profile must be an object")
        panel, band = spec.get("panel"), spec.get("band")
        family = spec.get("waveform_family", "0cnspq1tl" if panel == "266" else "154")
        if (not isinstance(panel, str) or not isinstance(family, str) or type(band) is not int
                or (panel, family, band) not in PROFILES):
            raise ValueError("supported profiles are 154/6, 154/7, 266/0cnspq1tl/6, 266/0enspq1yq/5")
        if panel in seen:
            raise ValueError("provide only one fixed waveform band per panel")
        seen.add(panel)
        panel_id, minimum, maximum, extension, fingerprints = PROFILES[(panel, family, band)]
        record_bytes = WAVE_BYTES + len(bytes.fromhex(extension))
        try:
            temperature = Decimal(str(spec["temperature_c"])) * 100
        except (KeyError, InvalidOperation):
            raise ValueError("each profile needs a finite bench temperature_c") from None
        if not temperature.is_finite() or temperature != temperature.to_integral_value() or not minimum <= temperature <= maximum:
            raise ValueError("bench temperature must fit the selected provisional band to 0.01 C")
        if ("records" in spec) == ("backup" in spec):
            raise ValueError("choose records or backup, exclusively")
        source_hash = None
        if "backup" in spec:
            if not isinstance(spec["backup"], str):
                raise ValueError("backup must name a file")
            capture = (manifest_path.parent / spec["backup"]).read_bytes()
            if len(capture) != 512 * 1024:
                raise ValueError("backup must be an exact 512 KiB factory capture")
            source_hash = hashlib.sha256(capture).hexdigest()
            addresses = [0x78188 + (2 * band + component) * record_bytes for component in range(2)]
            records = [capture[address:address + record_bytes] for address in addresses]
        else:
            paths = spec["records"]
            if not isinstance(paths, list) or len(paths) != 2 or any(not isinstance(path, str) for path in paths):
                raise ValueError("records must name exactly two complete component files")
            records = [(manifest_path.parent / path).read_bytes() for path in paths]
        for component, record in enumerate(records):
            if len(record) != record_bytes or record[WAVE_BYTES:] != bytes.fromhex(extension):
                raise ValueError("expected complete records with the selected captured command extension")
            if hashlib.sha256(record).hexdigest() != fingerprints[component]:
                raise ValueError("waveform fingerprint does not match the selected captured panel/band")
        result.append({"panel": panel, "waveform_family": family, "panel_id": panel_id, "band": band,
                       "minimum": minimum, "maximum": maximum, "temperature_centi": int(temperature),
                       "records": records, "record_sha256": list(fingerprints), "source_sha256": source_hash})
    return result


def render_provider(profiles):
    lines = ['/* Private owner-provisioned data; redistribution rights unresolved. */',
             '#include "hanshow_qn.h"', '']
    for profile in profiles:
        name = "qn" + profile["panel"]
        for component, record in enumerate(profile["records"]):
            lines.append(f'static const uint8_t {name}_pass{component}[HANSHOW_QN_WAVE_BYTES] = {{')
            for offset in range(0, WAVE_BYTES, 16):
                lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in record[offset:min(offset + 16, WAVE_BYTES)]) + ",")
            lines.append("};")
        explicit = profile["waveform_family"] == "0enspq1yq"
        if explicit:
            for component, record in enumerate(profile["records"]):
                extension = record[WAVE_BYTES:]
                lines.append(f'static const struct hanshow_qn_initializer {name}_init{component} = {{')
                for field, start, count in (("booster", 4, 7), ("timing", 13, 2), ("gate_start", 17, 4)):
                    values = ", ".join(f"0x{byte:02x}" for byte in extension[start:start + count])
                    lines.append(f'    .{field} = {{ {values} }},')
                lines.append("};")
        lines.append(f'static const struct hanshow_qn_waveforms {name} = {{')
        lines.append(f'    .panel_id = {profile["panel_id"]}u, .band = {profile["band"]}u,')
        lines.append(f'    .min_centi = {profile["minimum"]}, .max_centi = {profile["maximum"]},')
        lines.append(f'    .pass = {{ {name}_pass0, {name}_pass1 }},')
        if explicit:
            lines.append('    .family = HANSHOW_QN_FAMILY_0ENSPQ1YQ,')
            lines.append(f'    .initializer = {{ &{name}_init0, &{name}_init1 }}')
        lines.extend(["};", ""])
    lines.extend(['const struct hanshow_qn_waveforms *hanshow_qn_waveforms_for(uint16_t panel_id)', '{'])
    for profile in profiles:
        lines.append(f'    if (panel_id == {profile["panel_id"]}u) return &qn{profile["panel"]};')
    lines.extend(["    (void)panel_id;", "    return NULL;", "}", ""])
    return "\n".join(lines)


def write_private_output(output, profiles):
    output = Path(output).resolve()
    if output == REPOSITORY or REPOSITORY in output.parents:
        raise ValueError("private waveform output must be outside the repository")
    # Reserve a fresh private directory; never overwrite previous provenance.
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    provenance = {"manufacturer_derived": True, "redistribution_rights": "unresolved",
                  "thermal_policy": "owner-selected fixed band; requires manual bench qualification",
                  "hardware_validated_by_this_tool": False,
                  "profiles": [{key: value for key, value in profile.items() if key != "records"} for profile in profiles]}
    for name, content in (("hanshow_waveforms.c", render_provider(profiles)),
                          ("provenance.json", json.dumps(provenance, indent=2) + "\n")):
        path = output / name
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            stream.write(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, help="private explicit profile JSON; omission generates an unavailable provider")
    parser.add_argument("--output", type=Path, required=True, help="new private directory outside the repository")
    args = parser.parse_args()
    try:
        profiles = load_profiles(args.manifest) if args.manifest else []
        write_private_output(args.output, profiles)
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))
    print("Private native provider generated; no hardware accessed. Keep output and waveform-bearing binaries private.")


if __name__ == "__main__":
    main()
