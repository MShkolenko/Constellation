#!/bin/bash
# Кривая: сколько квестов даёт спутник на СВОЙ прожитый час, по уровням.
#
# Оператор, 2026-09-08: «некорректно сравнивать только что созданных персонажей и их же спустя
# десятки часов, вначале прогресс заметнее». Верно, и это надо превратить в число, а не принять
# на слово: если сдача на прожитый час падает с уровнем, то падение общей скорости состава —
# форма кривой, а не поломка.
#
# `characters.totaltime` — прожитое время персонажа в секундах, ровно то, что нужно для
# нормировки. Отметок времени у сданных квестов в базе нет, поэтому это средняя за жизнь,
# а не мгновенная скорость: для сравнения когорт по возрасту этого достаточно.
BOTS="(SELECT a.id FROM auth_1127.account a JOIN auth_1127.battlenet_accounts b ON b.id=a.battlenet_account WHERE b.email LIKE 'CONSTELLATION%')"

echo "=== сдано на прожитый час, по уровням (весь состав) ==="
mysql -t -e "
SELECT
  CASE WHEN c.level <= 2 THEN '1-2'
       WHEN c.level <= 4 THEN '3-4'
       WHEN c.level <= 6 THEN '5-6'
       WHEN c.level <= 8 THEN '7-8'
       ELSE '9+' END AS ур,
  COUNT(*) AS душ,
  ROUND(AVG(c.totaltime)/3600, 2) AS ср_часов,
  ROUND(AVG((SELECT COUNT(*) FROM characters_1127.character_queststatus_rewarded r WHERE r.guid=c.guid)), 1) AS ср_сдано,
  ROUND(SUM((SELECT COUNT(*) FROM characters_1127.character_queststatus_rewarded r WHERE r.guid=c.guid))
        / (SUM(c.totaltime)/3600), 2) AS сдано_в_час
FROM characters_1127.characters c
WHERE c.account IN $BOTS AND c.totaltime > 600
GROUP BY ур ORDER BY MIN(c.level);"

echo
echo "=== то же по классам-исключениям, чтобы ДК и ДХ не искажали ==="
mysql -t -e "
SELECT CASE WHEN c.class=6 THEN 'ДК' WHEN c.class=12 THEN 'ДХ' ELSE 'прочие' END AS кто,
  COUNT(*) душ, ROUND(AVG(c.level),1) ср_ур, ROUND(AVG(c.totaltime)/3600,2) ср_часов,
  ROUND(SUM((SELECT COUNT(*) FROM characters_1127.character_queststatus_rewarded r WHERE r.guid=c.guid))
        / (SUM(c.totaltime)/3600), 2) сдано_в_час
FROM characters_1127.characters c
WHERE c.account IN $BOTS AND c.totaltime > 600
GROUP BY кто;"

echo
echo "=== разброс: пятеро лучших и пятеро худших по сдаче в час ==="
mysql -t -e "
SELECT c.name, c.level ур, ROUND(c.totaltime/3600,2) часов,
  (SELECT COUNT(*) FROM characters_1127.character_queststatus_rewarded r WHERE r.guid=c.guid) сдано,
  ROUND((SELECT COUNT(*) FROM characters_1127.character_queststatus_rewarded r WHERE r.guid=c.guid)/(c.totaltime/3600),2) в_час
FROM characters_1127.characters c
WHERE c.account IN $BOTS AND c.totaltime > 3600 AND c.class NOT IN (6,12)
ORDER BY в_час DESC LIMIT 5;"
mysql -t -e "
SELECT c.name, c.level ур, ROUND(c.totaltime/3600,2) часов,
  (SELECT COUNT(*) FROM characters_1127.character_queststatus_rewarded r WHERE r.guid=c.guid) сдано,
  ROUND((SELECT COUNT(*) FROM characters_1127.character_queststatus_rewarded r WHERE r.guid=c.guid)/(c.totaltime/3600),2) в_час
FROM characters_1127.characters c
WHERE c.account IN $BOTS AND c.totaltime > 3600 AND c.class NOT IN (6,12)
ORDER BY в_час ASC LIMIT 5;"
