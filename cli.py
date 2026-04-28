from __future__ import annotations

import argparse
import json
from pathlib import Path

from .config import profile_config
from .field import load_scene, write_json
from .manager import build_default_rechts_demo
from .mesh import extract_mesh_marching_tetrahedra
from .qa import strict_validation_gate, package_value_score
from .smoke_press import run_smoke_press


def cmd_demo(args: argparse.Namespace) -> None:
    cfg = profile_config(args.profile).with_resolution(tuple(args.resolution) if args.resolution else None)
    if args.relaxed:
        cfg = cfg.relaxed()
    im = build_default_rechts_demo(cfg)
    manifest = im.export_all(args.out)
    print(json.dumps({
        "ok": True,
        "manifest": manifest["outputs"]["manifest"],
        "package": manifest["outputs"]["package"],
        "mesh_validation": manifest["mesh_validation"],
        "package_value_score": manifest["package_value_score"],
    }, indent=2))


def cmd_press(args: argparse.Namespace) -> None:
    report = run_smoke_press(
        out_dir=args.out,
        resolution=tuple(args.resolution),
        profile=args.profile,
    )
    print(json.dumps({
        "ok": True,
        "pressure_verdict": report["pressure_verdict"],
        "smoke_press_value": report["outputs"]["smoke_press_value"],
        "mesh_validation": report["mesh_validation"],
        "package_value_score": report["package_value_score"],
    }, indent=2))


def cmd_build(args: argparse.Namespace) -> None:
    cfg = profile_config(args.profile).with_resolution(tuple(args.resolution) if args.resolution else None)
    if args.relaxed:
        cfg = cfg.relaxed()

    meta_map, primitives, source_scene = load_scene(args.scene, override_resolution=cfg.resolution)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    meta, build_report = meta_map.build(primitives, cfg)
    package_path = out / "package.npz"
    meta.save_npz(package_path)

    validation = None
    mesh_outputs = {}

    if args.mesh:
        mesh = extract_mesh_marching_tetrahedra(
            meta.sdf,
            meta.bounds_min,
            meta.bounds_max,
            iso=cfg.iso,
            weld_precision=cfg.weld_precision,
            solidify=cfg.solidify_mesh,
        )
        validation = mesh.validation_report()
        ok, reasons = strict_validation_gate(validation, cfg.min_solid_rechts_score)
        validation["strict_gate_passed"] = bool(ok)
        validation["strict_gate_reasons"] = reasons
        if cfg.strict_solid_gate and not ok:
            write_json(out / "validation.json", validation)
            raise RuntimeError("Strict solid gate failed: " + "; ".join(reasons))

        if cfg.export_obj:
            obj = out / "mesh.obj"
            mesh.save_obj(obj)
            mesh_outputs["obj"] = str(obj)
        if cfg.export_ply:
            ply = out / "mesh.ply"
            mesh.save_ply(ply)
            mesh_outputs["ply"] = str(ply)
        if cfg.export_stl:
            stl = out / "mesh.stl"
            mesh.save_stl_ascii(stl)
            mesh_outputs["stl"] = str(stl)
        write_json(out / "validation.json", validation)

    write_json(out / "scene.json", source_scene)
    meta_stats = meta.stats()
    value = package_value_score(meta_stats, validation) if validation else None

    manifest = {
        "format": "sdfpack-rechts-v3-output",
        "profile": cfg.mode,
        "build_report": build_report,
        "meta_stats": meta_stats,
        "mesh_validation": validation,
        "package_value_score": value,
        "outputs": {
            "package": str(package_path),
            "scene": str(out / "scene.json"),
            "validation": str(out / "validation.json") if validation else None,
            "meshes": mesh_outputs,
            "manifest": str(out / "manifest.json"),
        },
    }
    write_json(out / "manifest.json", manifest)

    print(json.dumps({
        "ok": True,
        "manifest": str(out / "manifest.json"),
        "package": str(package_path),
        "mesh_validation": validation,
        "package_value_score": value,
    }, indent=2))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="SDF Prepackager Rechts v3 with smoke-press validation.")
    sub = p.add_subparsers(required=True)

    d = sub.add_parser("demo", help="Build the default v3 package.")
    d.add_argument("--out", default="out_demo")
    d.add_argument("--profile", default="game", choices=["debug", "game", "collision", "high"])
    d.add_argument("--resolution", nargs=3, type=int, default=None)
    d.add_argument("--relaxed", action="store_true", help="Do not fail on strict mesh gate.")
    d.set_defaults(func=cmd_demo)

    press = sub.add_parser("press", help="Run smoke press and produce smoke_press_value.json.")
    press.add_argument("--out", default="out_press")
    press.add_argument("--profile", default="debug", choices=["debug", "game", "collision", "high"])
    press.add_argument("--resolution", nargs=3, type=int, default=(28, 28, 28))
    press.set_defaults(func=cmd_press)

    b = sub.add_parser("build", help="Build from a scene JSON file.")
    b.add_argument("scene")
    b.add_argument("--out", default="out_scene")
    b.add_argument("--profile", default="game", choices=["debug", "game", "collision", "high"])
    b.add_argument("--resolution", nargs=3, type=int, default=None)
    b.add_argument("--mesh", action="store_true")
    b.add_argument("--relaxed", action="store_true", help="Do not fail on strict mesh gate.")
    b.set_defaults(func=cmd_build)

    return p


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
