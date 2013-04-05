{
    "targets": [
        {
            "target_name": "zephyr",
            "sources": [ "src/zephyr.cc" ],
            "link_settings": {
                "libraries": [
                    "-lzephyr"
                ]
            },
            "cflags": [
                "-W", "-Wall", "-Wno-unused-parameter"
            ]
        }
    ]
}
