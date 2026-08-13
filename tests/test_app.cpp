#include <memory>
#include <string>

#include "app/app.hpp"
#include "app/catalog.hpp"
#include "apps/bloc.hpp"
#include <vector>

#include "harness.hpp"
#include "render/surface.hpp"

using sshos::App;
using sshos::Bloc;
using sshos::CloseCheck;
using sshos::Host;
using sshos::KeyEvent;
using sshos::MouseEvent;
using sshos::Pos;
using sshos::Rect;
using sshos::Size;
using sshos::Style;
using sshos::Surface;
using sshos::View;

namespace {

// Double d'hôte : enregistre ce qu'une application demande, sans rien
// exécuter. Suffit à tout le contrat sauf watch/unwatch, exercés à la
// tâche 8 avec un vrai registrar.
struct FakeHost : Host {
  std::string title;
  int close_requests = 0;
  int invalidations = 0;
  uint64_t next_token = 100;

  void set_title(std::string t) override { title = std::move(t); }
  void request_close() override { ++close_requests; }
  void invalidate() override { ++invalidations; }
  uint64_t watch(int, uint32_t) override { return next_token++; }
  void unwatch(uint64_t) override {}
  void watch_child(pid_t p) override { children.push_back(p); }
  std::vector<pid_t> children;
};

std::string row_of(const Surface& s, int y) { return s.text_row(y); }

}  // namespace

TEST(bloc_announces_its_size_after_a_resize) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  app.on_resize(Size{20, 6});

  Surface s(20, 6);
  View v = s.root();
  app.render(v);
  CHECK(row_of(s, 0).find("taille: 20x6") != std::string::npos);
}

// L'observable qui remplace le compteur du bouchon de session : les trois
// tests du round EPOLLHUP comptent des clics et vérifient qu'ils survivent
// au rattachement (voir la section « migration des tests » du plan).
TEST(bloc_counts_the_presses_it_receives) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  app.on_resize(Size{20, 6});

  Surface s(20, 6);
  {
    View v = s.root();
    app.render(v);
    CHECK(row_of(s, 1).find("clics: 0") != std::string::npos);
  }

  MouseEvent m;
  m.action = sshos::MouseAction::Press;
  m.x = 3;
  m.y = 2;
  app.on_mouse(m);
  app.on_mouse(m);
  CHECK_EQ(app.click_count(), 2);

  s.clear(Style{});
  View v2 = s.root();
  app.render(v2);
  CHECK(row_of(s, 1).find("clics: 2") != std::string::npos);
}

// Le curseur inverse la cellule sous lui plutôt que de la remplacer. Sans
// cette propriété, le curseur au repos -- en (0,0) -- effacerait le « t »
// de « taille: », et tout relevé de ligne passant sous lui deviendrait
// illisible : c'est exactement ce dont dépendent les tests de session.
TEST(bloc_cursor_highlights_the_glyph_under_it_instead_of_erasing_it) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  app.on_resize(Size{20, 6});

  Surface s(20, 6);
  View v = s.root();
  app.render(v);
  CHECK_EQ(s.at(0, 0).ch, U't');
  CHECK((s.at(0, 0).attrs & sshos::attr::Reverse) != 0);
  CHECK((s.at(1, 0).attrs & sshos::attr::Reverse) == 0);
}

// Le relevé qui sert de preuve à la tâche 5 : un geste de redimensionnement
// entier ne doit produire qu'UN appel, pas un par pixel parcouru.
TEST(bloc_counts_its_resizes) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  CHECK_EQ(app.resize_count(), 0);
  app.on_resize(Size{20, 6});
  app.on_resize(Size{30, 8});
  CHECK_EQ(app.resize_count(), 2);
}

TEST(bloc_moves_its_cursor_with_the_arrows_and_the_mouse) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  app.on_resize(Size{20, 6});

  Pos p;
  CHECK(app.wants_cursor(p));
  CHECK_EQ(p.x, 0);
  CHECK_EQ(p.y, 0);

  app.on_key(KeyEvent{sshos::Key::Right, 0, 0});
  app.on_key(KeyEvent{sshos::Key::Down, 0, 0});
  CHECK(app.wants_cursor(p));
  CHECK_EQ(p.x, 1);
  CHECK_EQ(p.y, 1);

  MouseEvent m;
  m.action = sshos::MouseAction::Press;
  m.x = 7;
  m.y = 3;
  app.on_mouse(m);
  CHECK(app.wants_cursor(p));
  CHECK_EQ(p.x, 7);
  CHECK_EQ(p.y, 3);
}

// Le curseur reste dans la zone cliente même quand elle rétrécit sous lui :
// sinon la position transmise au client désignerait une cellule qui
// n'existe plus, et le terminal placerait son curseur n'importe où.
TEST(bloc_keeps_its_cursor_inside_a_shrinking_client_area) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  app.on_resize(Size{20, 6});
  app.on_key(KeyEvent{sshos::Key::End, 0, 0});

  app.on_resize(Size{5, 2});
  Pos p;
  CHECK(app.wants_cursor(p));
  CHECK(p.x < 5);
  CHECK(p.y < 2);
}

TEST(bloc_sets_its_own_title_on_attach) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  CHECK_EQ(host.title, std::string("Bloc"));
}

TEST(bloc_becomes_modified_on_the_first_keystroke_and_refuses_to_close) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  app.on_resize(Size{20, 6});

  CloseCheck before = app.can_close();
  CHECK(before.allowed);

  app.on_key(KeyEvent{sshos::Key::Char, U'a', 0});
  CloseCheck after = app.can_close();
  CHECK(!after.allowed);
  CHECK(after.question.find("Bloc") != std::string::npos);
  CHECK(host.title.find('*') != std::string::npos);
}

// Un simple déplacement du curseur ne « modifie » rien : sans cette
// distinction, ouvrir une fenêtre et appuyer une fois sur une flèche
// suffirait à déclencher un dialogue de confirmation à la fermeture.
TEST(bloc_is_not_modified_by_a_mere_cursor_move) {
  Bloc app;
  FakeHost host;
  app.attach(host);
  app.on_resize(Size{20, 6});
  app.on_key(KeyEvent{sshos::Key::Right, 0, 0});
  CHECK(app.can_close().allowed);
}

TEST(bloc_declares_a_minimum_size) {
  Bloc app;
  CHECK_EQ(app.min_size().w, 14);
  CHECK_EQ(app.min_size().h, 3);
}

TEST(catalog_lists_bloc_and_can_build_it) {
  const auto& entries = sshos::catalog();
  CHECK(!entries.empty());
  const auto* e = sshos::catalog_find("bloc");
  REQUIRE(e != nullptr);
  CHECK_EQ(e->label, std::string("Bloc"));
  std::unique_ptr<App> made = e->make();
  CHECK(made != nullptr);
  CHECK_EQ(made->min_size().w, 14);
}

TEST(catalog_returns_null_for_an_unknown_identifier) {
  CHECK(sshos::catalog_find("il-n-y-a-personne") == nullptr);
}

// Le contrat par défaut doit être utilisable tel quel : une application qui
// n'implémente que render() doit compiler et se comporter raisonnablement.
TEST(app_defaults_are_usable_without_overriding_anything) {
  struct Minimal : App {
    void render(View) override {}
  };
  Minimal m;
  Pos p{9, 9};
  CHECK(!m.wants_cursor(p));
  CHECK(m.can_close().allowed);
  CHECK(m.min_size().w > 0);
  CHECK(m.on_io(0, 0) == sshos::IoStatus::Ok);
}
