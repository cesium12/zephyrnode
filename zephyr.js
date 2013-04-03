var events = require('events');

var internal = require('./build/Release/zephyr');

var zephyr = new events.EventEmitter();
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
