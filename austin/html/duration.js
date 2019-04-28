// ---- Update the duration label in the status bar. ----

var start_time = Date.now();

var durationInterval = setInterval(
  function () {
    var duration = ((Date.now() - start_time)/1000).toString().toHHMMSS()
    document.getElementById("duration").innerHTML = duration;
  },
  1000
)
