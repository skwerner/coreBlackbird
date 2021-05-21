# CMake 

## Pixar USD Dependencies

Cycles can be build against _USD_ that has been build using `build_usd.py` script. Before that happens
`embree` version has to be updated to version 9. This can be done by editing `build_usd.py`
and replacing version to at least version 9. Next we can run the building script:

```
EMBREE_URL = "https://github.com/embree/embree/archive/v3.9.0.tar.gz"
```

And extra flags required by Cycles:

```
'-DEMBREE_RAY_MASK=ON',
'-DEMBREE_FILTER_FUNCTION=ON',
'-DEMBREE_GEOMETRY_CURVE=ON',
'-DEMBREE_GEOMETRY_TRIANGLE=ON',
'-DEMBREE_BACKFACE_CULLING=OFF'
```

```
python build_usd.py --verbose \ 
    --openvdb --opencolorio --openimageio --embree \
    --no-tutorials --no-examples <install_path>
```

To build Cycles with Pixar libraries:

```
cmake -DBUILD_WITH_REZ=OFF \
    -DBUILD_WITH_USD=ON -DUSD_ROOT=<install_path> -DCMAKE_INSTALL_PREFIX=<install_path>
```

It is possible to build _USD_ with debug by adding `--debug` flag.