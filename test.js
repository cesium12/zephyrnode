var zephyr = require('./build/Release/zephyr');
console.dir(zephyr);
zephyr.subscribe([ [ 'cesium', '*' ], [ 'ennesby', '*', '*' ] ], function(arg) {
    console.log('subscribed to', arg);
    zephyr.subscribe([], function(arg) {
        zephyr.subs(function(err, subs) {
            console.log('subscribed to', arguments);
        });
    });
});
zephyr.check(function(err, msg) {
    console.log(msg.class, msg.instance, msg.sender, msg.opcode, msg.signature, msg.message);
});
