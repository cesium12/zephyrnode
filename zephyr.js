var events = require('events');

var internal = require('./build/Release/zephyr');

var zephyr = new events.EventEmitter();
zephyr.sender = internal.sender;
zephyr.realm = internal.realm;
zephyr.subscribeTo = internal.subscribeTo;
zephyr.subs = internal.subs;
zephyr.sendNotice = internal.sendNotice;

internal.setMessageCallback(function(err, msg) {
  if (err) {
    zephyr.emit("error", err);
  } else {
    zephyr.emit("message", msg);
  }
});

module.exports = zephyr;
