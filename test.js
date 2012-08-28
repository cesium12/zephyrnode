var zephyr = require('./build/Release/zephyr');
console.dir(zephyr);

var cls = process.argv[2];
var inst = process.argv[3];

zephyr.subscribe([ [ cls, inst, '*' ] ], function() {
    zephyr.check(function(err, msg) {
        console.log("%s / %s / %s [%s] (%s)\n%s",
            msg.class, msg.instance, msg.sender,
            msg.opcode, msg.signature, msg.message);
    });
    process.stdin.on('data', function(message) {
        zephyr.send({
            class: cls,
            signature: 'badass rockstar zephyr',
            message: message
        }, function() {});
    });
    process.stdin.setEncoding('utf8');
    process.stdin.resume();
});
