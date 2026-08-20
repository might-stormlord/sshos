#!/bin/sh
# Le numero de version d'un commit : MAJEUR.MINEUR
#
# LE MAJEUR EST DECLARE, LE MINEUR EST COMPTE.
#
# Le majeur vit dans le fichier VERSION, a la racine. C'est le seul chiffre
# que quelqu'un decide : on l'incremente le jour ou le bureau change assez
# pour que ca se dise.
#
# Le mineur est le nombre de commits depuis que VERSION a pris sa valeur
# actuelle. Il repart donc a zero a chaque majeur -- comme on l'attend d'un
# « 2.0 » -- et il avance tout seul le reste du temps. Aucun fichier a
# maintenir, aucune discipline a tenir, et il ne peut pas reculer.
#
# Les empreintes git restent dans le fichier d'etat, pour les outils. Elles
# ne sont simplement plus ce qu'on montre : « cce9d11 -> 3512ffe » ne dit
# rien a personne, « 1.12 -> 1.13 » se lit.
#
# usage : version.sh [<depot>] [<commit>]
set -eu

REPO="${1:-.}"
REF="${2:-HEAD}"

major=$(git -C "$REPO" show "$REF:VERSION" 2>/dev/null | tr -dc '0-9') || major=""
[ -n "$major" ] || major=0

# Le dernier commit, EN REMONTANT DEPUIS REF, qui a touche VERSION. En
# limitant a l'ancetre : sinon un commit plus recent sur une autre branche
# fausserait le compte.
# UN CLONE SUPERFICIEL NE PEUT PAS COMPTER, ET DOIT LE DIRE. La tete greffee
# n'a pas de parent : git la prend pour une racine, `log -1 -- VERSION` la
# rend elle-meme, et le mineur tomberait a zero sans que rien ne le signale --
# « version 1.0 » pour toujours. Mieux vaut ne rien rendre : l'appelant verra
# une version vide et se taira, plutot que d'annoncer un chiffre faux. Ce
# script est copie sur la machine de chaque utilisateur ; il ne peut pas
# dependre de la prudence de son appelant.
if [ "$(git -C "$REPO" rev-parse --is-shallow-repository 2>/dev/null)" = true ]; then
  echo "version.sh: depot superficiel, le mineur n'est pas calculable" >&2
  exit 1
fi

base=$(git -C "$REPO" log --format=%H -1 "$REF" -- VERSION 2>/dev/null || true)

if [ -n "$base" ]; then
  minor=$(git -C "$REPO" rev-list --count "$base..$REF" 2>/dev/null || echo 0)
else
  # VERSION n'existe pas encore dans cette histoire : le mineur est alors le
  # nombre total de commits, ce qui reste monotone.
  minor=$(git -C "$REPO" rev-list --count "$REF" 2>/dev/null || echo 0)
fi

printf '%s.%s\n' "$major" "$minor"
