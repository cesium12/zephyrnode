var zephyr = require('./build/Release/zephyr');
console.dir(zephyr);
console.log(zephyr.subscribe([ 'cesium', '*' ], function(arg) {
    console.log('subscribed to', arg);
}));
