from sdfpack.config import profile_config
from sdfpack.manager import build_default_rechts_demo
from sdfpack.smoke_press import run_smoke_press


def test_rechts_demo_builds_meta_and_valid_mesh(tmp_path):
    cfg = profile_config("debug").with_resolution((24, 24, 24))
    im = build_default_rechts_demo(cfg)
    meta = im.build_meta()
    mesh = im.build_mesh()
    report = mesh.validation_report()

    assert meta.sdf.shape == (24, 24, 24)
    assert meta.gradient.shape == (24, 24, 24, 3)
    assert meta.normal.shape == (24, 24, 24, 3)
    assert meta.value_pressure.shape == (24, 24, 24)
    assert meta.moat.shape == (24, 24, 24)
    assert meta.mip_sdf_1.ndim == 3
    assert report["closed"] is True
    assert report["manifold"] is True
    assert report["boundary_edge_count"] == 0
    assert report["nonmanifold_edge_count"] == 0
    assert report["solid_rechts_score"] >= cfg.min_solid_rechts_score


def test_smoke_press_value(tmp_path):
    report = run_smoke_press(tmp_path / "press", resolution=(22, 22, 22), profile="debug")
    assert report["pressure_verdict"] == "PASS"
    assert report["package_value_score"] >= 0.85
    assert report["mesh_validation"]["strict_gate_passed"] is True
