
$(document).ready(function () {
  var $window = $(window);
  setTimeout(function () {
    $('#sidenav').affix({
      offset: {
        top: function () { $window.width() <= 980 ? 70 : 0 },
        bottom: 60
      }
    })
  }, 100)

var doc = $('#doc');
var html = marked(doc.html(), {
  highlight: function (code, lang) {
    var res = prettyPrintOne(code, lang);
    if (res) {
      return res;
    }
    return code;
  }
});
var p = doc.parentElement;
doc.replaceWith($(html));

// Populate the side nav
var nav = $('#sidenav');
var headings = {};

$('h1,h2,h3,h4,h5,h6', p).each(function () {
  var id = $(this).text();

  $(this).attr('id', id);

  nav.append(
    $('<li/>').append(
      $('<a/>', {href: '#'+id}).text(
        $(this).text()
      )
    )
  );
});

});
