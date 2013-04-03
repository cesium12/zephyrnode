var zephyr = require('./zephyr');
console.dir(zephyr);

var cls = process.argv[2];
var inst = process.argv[3];

zephyr.subscribeTo([ [ cls, inst, '*' ] ], function(err) {
  if (err) {
    console.dir(err);
    return;
  }

  zephyr.on("message", function(msg) {
    console.log("%s / %s / %s [%s] (%s)\n%s",
		msg.class, msg.instance, msg.sender,
		msg.opcode, msg.signature, msg.message);
  });
  process.stdin.on('data', function(message) {
    zephyr.sendNotice({
      class: cls,
      instance: inst,
      signature: 'badass rockstar zephyr',
      message: message
    }, function(err) {
      if (err) {
	console.dir(err);
	return;
      }
    });
  });
  process.stdin.setEncoding('utf8');
  process.stdin.resume();
});
