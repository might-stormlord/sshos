#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "apps/editor/editor.hpp"
#include "harness.hpp"
#include "render/surface.hpp"

using sshos::Editor;
using sshos::Key;
using sshos::KeyEvent;
using sshos::Size;
using sshos::TextPos;
namespace mod = sshos::mod;

namespace {

class TempFile {
 public:
  explicit TempFile(const std::string& content) {
    char tpl[] = "/tmp/sshos-edit-XXXXXX";
    const int fd = ::mkstemp(tpl);
    if (fd < 0) return;
    path_ = tpl;
    if (!content.empty()) {
      const ssize_t n = ::write(fd, content.data(), content.size());
      (void)n;
    }
    ::close(fd);
  }
  ~TempFile() {
    if (!path_.empty()) ::unlink(path_.c_str());
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  const std::string& path() const { return path_; }
  bool valid() const { return !path_.empty(); }

  std::string read() const {
    std::string out;
    const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return out;
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return out;
  }

 private:
  std::string path_;
};

KeyEvent ch(char32_t c) { return KeyEvent{Key::Char, c, 0}; }
KeyEvent ctrl(char32_t c) { return KeyEvent{Key::Char, c, mod::Ctrl}; }
KeyEvent key(Key k) { return KeyEvent{k, 0, 0}; }

std::string painted(Editor& e, int w, int h) {
  sshos::Surface s(w, h);
  e.render(sshos::View(s, sshos::Rect{0, 0, w, h}));
  std::string out;
  for (int y = 0; y < h; ++y) {
    if (y != 0) out.push_back('/');
    std::string row = s.text_row(y);
    while (!row.empty() && row.back() == ' ') row.pop_back();
    out += row;
  }
  return out;
}

struct MuteHost : sshos::Host {
  int close_requests = 0;
  std::string title;
  void set_title(std::string t) override { title = std::move(t); }
  void request_close() override { ++close_requests; }
  void invalidate() override {}
  uint64_t watch(int, uint32_t) override { return 0; }
  void unwatch(uint64_t) override {}
  void watch_child(pid_t) override {}
};

}  // namespace

// ------------------------------------------------------------ l'ouverture

TEST(editor_loads_the_file_it_was_given) {
  TempFile f("un\ndeux\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});

  CHECK_EQ(e.buffer_for_tests().line_count(), size_t{2});
  CHECK_EQ(e.buffer_for_tests().line(1), std::string("deux"));
  CHECK(!e.buffer_for_tests().modified());
}

TEST(editor_starts_empty_without_a_file) {
  Editor e;
  e.on_resize(Size{40, 10});
  CHECK_EQ(e.buffer_for_tests().line_count(), size_t{1});
}

// ------------------------------------------------------------- la saisie

TEST(editor_types_text_where_the_cursor_is) {
  Editor e;
  e.on_resize(Size{40, 10});
  e.on_key(ch(U'a'));
  e.on_key(ch(U'b'));

  CHECK_EQ(e.buffer_for_tests().line(0), std::string("ab"));
  CHECK_EQ(e.cursor_for_tests().col, size_t{2});
  CHECK(e.buffer_for_tests().modified());
}

TEST(editor_splits_the_line_on_enter) {
  Editor e;
  e.on_resize(Size{40, 10});
  e.on_key(ch(U'a'));
  e.on_key(key(Key::Enter));
  e.on_key(ch(U'b'));

  REQUIRE_EQ(e.buffer_for_tests().line_count(), size_t{2});
  CHECK_EQ(e.buffer_for_tests().line(1), std::string("b"));
  CHECK_EQ(e.cursor_for_tests().line, size_t{1});
}

TEST(editor_erases_backwards_and_forwards) {
  TempFile f("abc\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});
  e.on_key(key(Key::End));
  e.on_key(key(Key::Backspace));
  CHECK_EQ(e.buffer_for_tests().line(0), std::string("ab"));

  e.on_key(key(Key::Home));
  e.on_key(key(Key::Delete));
  CHECK_EQ(e.buffer_for_tests().line(0), std::string("b"));
}

// Un caractère de contrôle ne s'insère PAS : il donnerait un fichier
// qu'aucun autre éditeur ne relirait proprement.
TEST(editor_never_inserts_a_control_character) {
  Editor e;
  e.on_resize(Size{40, 10});
  e.on_key(KeyEvent{Key::Char, static_cast<char32_t>(7), 0});
  CHECK_EQ(e.buffer_for_tests().line(0), std::string(""));
}

// ------------------------------------------------------------ le curseur

TEST(editor_moves_the_cursor_with_the_arrows) {
  TempFile f("abc\ndef\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});

  e.on_key(key(Key::Down));
  CHECK_EQ(e.cursor_for_tests().line, size_t{1});
  e.on_key(key(Key::Right));
  CHECK_EQ(e.cursor_for_tests().col, size_t{1});
  e.on_key(key(Key::Up));
  CHECK_EQ(e.cursor_for_tests().line, size_t{0});
}

// Le curseur ne sort JAMAIS du tampon, ni par le haut ni par la droite
// d'une ligne plus courte.
TEST(editor_keeps_the_cursor_inside_the_buffer) {
  TempFile f("longue ligne\nab\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});

  e.on_key(key(Key::End));
  REQUIRE_EQ(e.cursor_for_tests().col, size_t{12});
  e.on_key(key(Key::Down));
  CHECK_EQ(e.cursor_for_tests().col, size_t{2});  // ramené au bout de « ab »

  e.on_key(key(Key::Up));
  e.on_key(key(Key::Up));
  CHECK_EQ(e.cursor_for_tests().line, size_t{0});
}

TEST(editor_scrolls_to_follow_the_cursor) {
  std::string big;
  for (int i = 0; i < 40; ++i) big += "ligne" + std::to_string(i) + "\n";
  TempFile f(big);
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 8});

  for (int i = 0; i < 30; ++i) e.on_key(key(Key::Down));
  CHECK(e.top_for_tests() > size_t{0});
  CHECK(e.cursor_for_tests().line >= e.top_for_tests());
}

// ----------------------------------------------------------- enregistrer

TEST(editor_saves_what_was_typed) {
  TempFile f("avant\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});
  e.on_key(key(Key::End));
  e.on_key(ch(U'!'));
  e.on_key(ctrl(U's'));

  CHECK_EQ(f.read(), std::string("avant!\n"));
  CHECK(!e.buffer_for_tests().modified());
}

// L'enregistrement rend le fichier TEL QU'IL ÉTAIT quand rien n'a changé
// -- absence de saut de ligne final comprise.
TEST(editor_saves_a_file_without_a_trailing_newline_as_it_was) {
  TempFile f("sans-saut");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});
  e.on_key(ctrl(U's'));

  CHECK_EQ(f.read(), std::string("sans-saut"));
}

TEST(editor_says_so_when_it_cannot_save) {
  Editor e("/proc/1/inexistant-xyz/fichier");
  e.on_resize(Size{40, 10});
  e.on_key(ch(U'a'));
  e.on_key(ctrl(U's'));

  CHECK(!e.status_for_tests().empty());
  CHECK(e.buffer_for_tests().modified());  // rien n'a été enregistré
}

// -------------------------------------------------------------- quitter

// `Ctrl+X` sur un tampon PROPRE ferme tout de suite.
TEST(editor_closes_at_once_when_nothing_changed) {
  TempFile f("a\n");
  REQUIRE(f.valid());
  MuteHost host;
  Editor e(f.path());
  e.on_resize(Size{40, 10});
  e.attach(host);

  e.on_key(ctrl(U'x'));
  CHECK_EQ(host.close_requests, 1);
}

// Modifié, il DEMANDE. Rien ne se perd sans question.
TEST(editor_asks_before_closing_a_modified_buffer) {
  TempFile f("a\n");
  REQUIRE(f.valid());
  MuteHost host;
  Editor e(f.path());
  e.on_resize(Size{40, 10});
  e.attach(host);
  e.on_key(ch(U'x'));

  e.on_key(ctrl(U'x'));
  CHECK_EQ(host.close_requests, 0);
  CHECK(e.mode_for_tests() == Editor::Mode::Confirming);

  e.on_key(ch(U'o'));
  CHECK_EQ(host.close_requests, 1);
}

TEST(editor_stays_open_when_the_answer_is_not_yes) {
  TempFile f("a\n");
  REQUIRE(f.valid());
  MuteHost host;
  Editor e(f.path());
  e.on_resize(Size{40, 10});
  e.attach(host);
  e.on_key(ch(U'x'));
  e.on_key(ctrl(U'x'));

  e.on_key(key(Key::Escape));
  CHECK_EQ(host.close_requests, 0);
  CHECK(e.mode_for_tests() == Editor::Mode::Normal);
}

// Le bureau demande aussi avant de fermer la fenêtre : un tampon modifié
// doit poser sa question là aussi, sinon le [×] du cadre perd le travail.
TEST(editor_refuses_a_window_close_while_it_is_modified) {
  TempFile f("a\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});
  CHECK(e.can_close().allowed);

  e.on_key(ch(U'z'));
  CHECK(!e.can_close().allowed);
}

// -------------------------------------------------------------- chercher

TEST(editor_searches_and_moves_the_cursor_to_what_it_found) {
  TempFile f("un\ndeux\ntrois\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});

  e.on_key(ctrl(U'f'));
  CHECK(e.mode_for_tests() == Editor::Mode::Searching);
  e.on_key(ch(U't'));
  e.on_key(ch(U'r'));
  e.on_key(key(Key::Enter));

  CHECK(e.mode_for_tests() == Editor::Mode::Normal);
  CHECK_EQ(e.cursor_for_tests().line, size_t{2});
}

TEST(editor_says_so_when_it_finds_nothing) {
  TempFile f("un\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});

  e.on_key(ctrl(U'f'));
  e.on_key(ch(U'z'));
  e.on_key(key(Key::Enter));

  CHECK(!e.status_for_tests().empty());
  CHECK_EQ(e.cursor_for_tests().line, size_t{0});
}

TEST(editor_cancels_a_search_on_escape) {
  TempFile f("un\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 10});

  e.on_key(ctrl(U'f'));
  e.on_key(ch(U'u'));
  e.on_key(key(Key::Escape));
  CHECK(e.mode_for_tests() == Editor::Mode::Normal);
  // Et la frappe suivante ÉCRIT, elle ne cherche plus.
  e.on_key(ch(U'x'));
  CHECK_EQ(e.buffer_for_tests().line(0), std::string("xun"));
}

// ---------------------------------------------------------------- le rendu

TEST(editor_paints_the_lines_of_the_buffer) {
  TempFile f("premiere\nseconde\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 8});

  const std::string g = painted(e, 40, 8);
  CHECK(g.find("premiere") != std::string::npos);
  CHECK(g.find("seconde") != std::string::npos);
}

// La ligne d'état dit le nom du fichier ET s'il est modifié : sans le
// second, on enregistre par superstition.
TEST(editor_shows_the_modified_marker) {
  TempFile f("a\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 8});
  CHECK(painted(e, 40, 8).find("*") == std::string::npos);

  e.on_key(ch(U'x'));
  CHECK(painted(e, 40, 8).find("*") != std::string::npos);
}

TEST(editor_puts_the_cursor_where_the_text_is) {
  TempFile f("abc\n");
  REQUIRE(f.valid());
  Editor e(f.path());
  e.on_resize(Size{40, 8});
  e.on_key(key(Key::Right));

  sshos::Pos p{};
  CHECK(e.wants_cursor(p));
  CHECK_EQ(p.x, 1);
  CHECK_EQ(p.y, 0);
}
