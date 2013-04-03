var events = require('events');

var internal = require('./build/Release/zephyr');

var zephyr = new events.EventEmitter();

zephyr.ZAUTH_FAILED = -1;
zephyr.ZAUTH_YES = 1;
zepyhr.ZAUTH_NO = 0;

zephyr.UNSAFE = 0;
zephyr.UNACKED = 1;
zephyr.ACKED = 2;
zephyr.HMACK = 3;
zephyr.HMCTL = 4;
zephyr.SERVACK = 5;
zephyr.SERVNAK = 6;
zephyr.CLIENTACK = 7;
zephyr.STAT = 8;

zephyr.sender = internal.sender;
zephyr.realm = internal.realm;
zephyr.subscribeTo = internal.subscribeTo;
zephyr.subs = internal.subs;
zephyr.sendNotice = function(msg, cb) {
  try {
    internal.sendNotice(msg);
  } catch (err) {
    process.nextTick(function() {
      cb(err);
    });
    return;
  }
  process.nextTick(function() {
    cb(null);
  });
}

internal.setMessageCallback(function(err, msg) {
  if (err) {
    zephyr.emit("error", err);
  } else {
    zephyr.emit("message", msg);
  }
});

module.exports = zephyr;
