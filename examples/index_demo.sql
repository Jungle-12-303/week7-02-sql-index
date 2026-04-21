INSERT INTO users (name, age) VALUES ('kim', 20);
INSERT INTO users (name, age) VALUES ('lee', 30);
INSERT INTO users (name, age) VALUES ('park', 25);
SELECT * FROM users WHERE id = 2;
SELECT * FROM users WHERE id > 2;
SELECT name, age FROM users WHERE id < 3;
SELECT name, age FROM users WHERE name = 'kim';
SELECT id, name FROM users WHERE age != 20;
SELECT * FROM users;
