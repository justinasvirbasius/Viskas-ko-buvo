from sdfpack import InterfaceManager, profile_config


def main() -> None:
    cfg = profile_config("game").with_resolution((40, 40, 40))
    im = InterfaceManager(bounds_min=(-2.5, -2.5, -2.5), bounds_max=(2.5, 2.5, 2.5), config=cfg)

    im.add_sphere(radius=1.0, material_id=1)
    im.add_box(center=(0.35, 0, 0), half_extents=(0.55, 0.7, 0.55), op="smooth_union", smooth_k=0.25, material_id=2)
    im.add_capsule(a=(-1, -0.6, -0.2), b=(1, 0.7, 0.5), radius=0.18, material_id=3)

    manifest = im.export_all("out_api")
    print(manifest["mesh_validation"])
    print("package_value_score:", manifest["package_value_score"])


if __name__ == "__main__":
    main()
