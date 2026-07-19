"""Verify that ScopeOne UI and Python share the same image scene state."""

from pprint import pprint

from scopeone import ScopeOne


def main() -> None:
    with ScopeOne() as scopeone:
        layers = scopeone.list_layers()
        if not layers:
            raise RuntimeError("No image layers are available. Start a ScopeOne preview first.")

        layer = next((item for item in layers if item.get("visible")), layers[0])
        layer_key = str(layer["layerKey"])
        print(f"Using layer: {layer_key}")

        markup_id = scopeone.create_line_markup(
            layer_key,
            20,
            20,
            150,
            100,
            label="Python scene sync",
            role="cross_section",
        )

        try:
            print("\nCreated markup:")
            pprint(scopeone.list_markups(layer_key))

            input(
                "\nDrag the line in the ScopeOne preview, then press Enter here "
                "to read the updated coordinates..."
            )

            markups = scopeone.list_markups(layer_key)
            current = next(
                (item for item in markups if item.get("id") == markup_id),
                None,
            )
            if current is None:
                raise RuntimeError("The test markup was removed from the ScopeOne UI.")

            print("\nMarkup after the UI edit:")
            pprint(current)

            updated = scopeone.update_markup(
                markup_id,
                label="Updated from Python",
                selected=True,
            )
            print("\nMarkup after the Python update:")
            pprint(updated)

            input("\nConfirm the updated label in ScopeOne, then press Enter to remove it...")
        finally:
            if any(item.get("id") == markup_id for item in scopeone.list_markups()):
                scopeone.remove_markup(markup_id)
                print("\nTest markup removed.")


if __name__ == "__main__":
    main()
