var idx = null;
var this_topic = null;
var titles_by_search_id = {};

function make_id(text) {
  return text.replace(/[ ]+/g, '-');
}

function build_search() {
  // If we find PH_ABC in the set of tokens, generate a fake
  // ABC token so that we don't need to keep on ph_'ing
  var tokenizer = lunr.tokenizer;
  lunr.tokenizer = function (input) {
    var tokens = tokenizer(input);
    var len = tokens.length;

    for (var i = 0; i < len; i++) {
      var m = tokens[i].match(/^ph_(.*)$/);
      if (m) {
        tokens.push(m[1]);
      }
    }
    return tokens;
  };

  idx = lunr(function() {
    this.ref('id');
    this.field('title', { boost: 100 });
    this.field('body');
  });

  // docs loaded from declmap.js
  for (k in docs) {
    var d = docs[k];

    // Partition each doc into section divided by headings
    var tokens = marked.lexer(d.content);

    var sofar = [];
    var title = d.title;

    function add_doc() {
      if (sofar.length) {
        var sid = '#' + d.name + '--' + make_id(title);
        titles_by_search_id[sid] = title;
        idx.add({
          id: sid,
          title: title,
          body: sofar.join(' ')
        });
        sofar = [];
      }
    }

    for (var i = 0; i < tokens.length; i++) {
      var t = tokens[i];
      if (t.type == 'heading') {
        add_doc();
        title = t.text;
        continue;
      }
      sofar.push(t.text);
    }

    add_doc();
  }

  // Use the typeahead to render search results as they type them
  var searchbox = $('.search-query');
  searchbox.typeahead({
    // Execut the query
    source: function (query, process) {
      var terms = idx.search(query);
      var res = []
      for (var i = 0; i < terms.length; i++) {
        res.push(terms[i].ref);
      }
      return res;
    },

    // Don't override the search function ordering
    sorter: function (items) {
      return items;
    },

    // Return the readable version of the text
    highlighter: function (item) {
      return titles_by_search_id[item];
    },

    // Always include results from the search function
    matcher: function (item) {
      return true;
    },

    // Send them to the right place, clear input, when they accept it
    updater: function (item) {
      // switch the content to the topic first ...
      show_topic_for_hash(item);
      // ... so that this can resolve the correct links and scroll the doc
      window.location.href = item;
      // shift focus away from the search box
      searchbox.blur();
      // clear the search box
      return '';
    }
  });

  $(document).on('keypress.phenomfocus', function(event) {
    if (event.target == document.body && event.keyCode != 32) {
      searchbox.focus();
      // Swallow '/', but otherwise allow the character into the
      // search box
      if (event.keyCode == 47) {
        return false;
      }
    }
  });
}

function show_topic(topic_name) {
  if (topic_name == this_topic) {
    return false;
  }

  var topic = docs[topic_name];
  this_topic = topic_name;

  $('head title').text(topic.title);

  var html = marked(topic.content, {
    highlight: function (code, lang) {
      // allow ```none to signal that we shouldn't highlight a block at all
      if (lang != 'none') {
        var res = prettyPrintOne(code, lang);
        if (res) {
          return res;
        }
      }
      return code;
    }
  });

  // Try to auto-link stuff in the content based on our declmap
  var keys = []
  for (k in declmap) {
    keys.push(k);
  }
  var munged = []
  var regex = new RegExp('(' + keys.join('|') + ')\\(\\)');
  var m;
  while ((m = regex.exec(html)) !== null) {
    var decl = m[1];

    // Take out the text before and append
    munged.push(html.substr(0, m.index));

    // Construct a link
    munged.push('<a href="#' + declmap[decl] + '--' + decl + '">' +
        decl + '()</a>');

    html = html.substr(m.index + m[0].length);
  }
  munged.push(html);
  html = munged.join('');

  var p = $('#doccontent');
  p.html(html);

  // Populate the side nav
  var nav = $('#sidenav');
  nav.empty();
  var headings = {};

  $('h1,h2,h3,h4,h5,h6', p).each(function () {
    var id = topic_name + '--' + make_id($(this).text());

    $(this).attr('id', id);

    var text = $(this).text();
    var li = $('<li/>');
    li.append(
      $('<a/>', {
        href: '#' + id,
        title: text
      }).text(text)
    );
    nav.append(li);
  });

  // Defer initializing scrollspy until now, and only call refresh
  // once we've initialized it once, otherwise we trigger an issue
  // where the wrong element is highlighted
  var body = $('body');
  if (body.data('scrollspy')) {
    body.scrollspy('refresh');
  } else {
    body.scrollspy(body.data());
  }

  // Fixup active status of topic nav
  var this_hash = '#' + topic_name;
  $('#topic-menu a').each(function () {
    if ($(this).attr('href') == this_hash) {
      $(this).parent().addClass('active');
    } else {
      $(this).parent().removeClass('active');
    }
  });
  return true;
}

function get_url_hash() {
  return location.href.replace(/^[^#]*#?(.*)$/, '$1').toString();
}

function show_topic_for_hash(hash) {
  var m = hash.match(/^#?(.+)--(.+)$/);
  if (m) {
    return show_topic(m[1]);
  }
  m = hash.match(/^#?(.+)$/);
  if (m) {
    return show_topic(m[1]);
  }

  return show_topic('README');
}

function hash_changed() {
  show_topic_for_hash(get_url_hash());
}

function setup_hashchange() {
  var doc_mode = document.documentMode;

  if ('onhashchange' in window && (doc_mode === undefined || doc_mode > 7)) {
    $(window).on('hashchange', hash_changed);
  } else {
    // Poll and fake it
    var last_hash = get_url_hash();

    function check_hash() {
      var hash = get_url_hash();
      if (last_hash != hash) {
        last_hash = hash;
        hash_changed();
      }
      setTimeout(check_hash, 50);
    }
    check_hash();
  }
  hash_changed();
}

// Populate the topic drop-down menu navigation.
// The active class is added by show_topic()
function load_topic_nav() {
  var nav = $('#topic-menu');

  for (k in docs) {
    var d = docs[k];

    var li = $('<li/>');
    var a = $('<a/>', { href: '#' + d.name });
    a.text(d.title);

    li.append(a);
    nav.append(li);
  }
}

$(document).ready(function () {
  load_topic_nav();
  setup_hashchange();
  build_search();
});


