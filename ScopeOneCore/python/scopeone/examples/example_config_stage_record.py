from scopeone import ScopeOne


CONFIG_PATH = r"C:\Users\T7910\Documents\Source\cpp\ScopeOne\config\MMConfig_demo.cfg"
SAVE_DIR = r"C:\Users\T7910\Downloads"


def main() -> None:
    with ScopeOne() as scope:
        scope.load_config(CONFIG_PATH)

        camera = scope.camera_ids()[0]
        scope.set_exposure(10.0, camera)

        scope.set_property(camera, "Mode", "Fluorescent Beads")

        stage = scope.current_xy_stage_device()
        print("Position:", scope.read_xy_position(stage))

        with scope.record(frames=10, camera=camera) as first:
            first.save(SAVE_DIR, "before_move", format="ome-tiff")

        scope.move_xy_relative(10.0, 0.0, stage)
        print("Position:", scope.read_xy_position(stage))

        with scope.record(frames=10, camera=camera) as second:
            second.save(SAVE_DIR, "after_move", format="ome-tiff")

        scope.unload_config()


if __name__ == "__main__":
    main()
