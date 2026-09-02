# Colormap LUTs

Each `.lut` file uses the standard 768-byte ImageJ layout: 256 red values,
followed by 256 green values and 256 blue values.

Gray and the single-channel or dual-channel tables are linear maps. `Fire.lut`
follows the ImageJ Fire map used by Micro-Manager, and `Ice.lut` follows the
ImageJ Ice map. The Viridis, Inferno, Magma, and Cividis tables use the canonical
listed colormaps published by Matplotlib.

`Jet.lut` is the classic MATLAB/ImageJ jet map, `Rainbow.lut` is a continuous
rainbow map, and `Turbo.lut` is Google's perceptually improved jet replacement.
