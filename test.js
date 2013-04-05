var zephyr = require('./zephyr');
console.dir(zephyr);

var cls = process.argv[2];
var inst = process.argv[3];

zephyr.subscribeTo([ [ cls, inst, '*' ] ], function(err) {
  if (err) {
    console.dir(err);
    return;
  }

  zephyr.on("notice", function(msg) {
    console.log("%s / %s / %s [%s] (%s)\n%s",
		msg.class, msg.instance, msg.sender,
		msg.opcode, msg.body[0], msg.body[1]);
  });
  process.stdin.on('data', function(message) {
    zephyr.sendNotice({
      class: cls,
      instance: inst,
      body: [
	'badass rockstar zephyr',
	message
      ]
    }, function(err) {
      if (err) {
	console.dir(err);
	return;
      }
      console.log('got HMACK');
    }).on('servack', function(err, msg) {
      if (err) {
	console.dir('got SERVNAK', err);
	return;
      }
      console.log('got SERVACK', msg);
    });
  });
  process.stdin.setEncoding('utf8');
  process.stdin.resume();
});
