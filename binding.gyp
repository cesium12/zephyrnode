{
    "targets": [
        {
            "target_name": "zephyr",
            "sources": [ "zephyr.cc" ],
            "link_settings": {
                "libraries": [
                    "-lzephyr"
                ]
            },
            "include_dirs": [
                "<!(node -e \"require('nan')\")"
            ],
            "cflags": [
                "-W", "-Wall", "-Wno-unused-parameter"
            ]
        }
    ]
}
