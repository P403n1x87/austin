// ---- Web Socket ----

function get_ws_url() {
  var loc = window.location, new_uri;
  if (loc.protocol === "https:") {
      new_uri = "wss:";
  } else {
      new_uri = "ws:";
  }
  return new_uri + "//" + loc.host + loc.pathname + "ws";
}

var webSocket = new WebSocket(get_ws_url());

// ---- On Message ----

webSocket.onmessage = function (event) {
  var payload = JSON.parse(event.data);

  switch (payload.type) {
  case "sample":
    flameGraph.setHeight(payload.height);
    flameGraph.setWidth(document.getElementById('chart').offsetWidth);
    flameGraph.merge(payload.data);
    document.getElementById('samples').innerHTML = payload.samples;
    document.getElementById('cpu').innerHTML = payload.cpu + "%";
    document.getElementById('memory').innerHTML = payload.memory + " MB";
    break;

  case "info":
    document.getElementById('pid').innerHTML = payload.pid;
    document.getElementById('command').innerHTML = payload.command;
  }
}

// ---- On Open ----

var isOpen = false;

webSocket.onopen = function (event) {
  isOpen = true;

  setStatusColor("green");
  dataInterval = setDataInterval();
}

// ---- On Close ----

webSocket.onclose = function (event) {
  setStatusColor("red");

  clearInterval(durationInterval);
  isOpen = false;
  // TODO: Disable play button
}

// ---- Data Interval ----

function setDataInterval() {
  webSocket.send("data");
  return setInterval(
    function () {
      webSocket.send("data");
    },
    3000
  );
}

function setStatusColor(color) {
  d3.select("#status")
    .classed("bg-green-700", color == "green")
    .classed("bg-red-700", color == "red")
    .classed("bg-yellow-700", color == "yellow");
}


function togglePlay() {
  if (!isOpen) {
    return;
  }

  if (isPlaying) {
    clearInterval(dataInterval);
    setStatusColor("yellow");
    d3.select(".fa-pause").classed("fa-play", true).classed("fa-pause", false);
  }
  else {
    dataInterval = setDataInterval();
    setStatusColor("green");
    d3.select(".fa-play").classed("fa-play", false).classed("fa-pause", true);
  }

  isPlaying = !isPlaying;
}

// ---- Init ----

var dataInterval;
var isPlaying = true;
