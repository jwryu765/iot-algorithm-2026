#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define TITLE_MAX     100
#define GENRE_MAX      50
#define DIRECTOR_MAX   60
#define LINE_MAX      256

typedef struct {
    int id;
    char title[TITLE_MAX];
    char genre[GENRE_MAX];
    char director[DIRECTOR_MAX];
    int year;
    int runtime;
    double rating;
} Movie;

/* ===========================
   Hash table (id -> index)
=========================== */
typedef enum { SLOT_EMPTY = 0, SLOT_FILLED = 1, SLOT_TOMB = 2 } SlotState;

typedef struct {
    int key;
    size_t value;
    SlotState state;
} HashSlot;

typedef struct {
    HashSlot* slots;
    size_t cap;
    size_t size;
    size_t tombs;
} HashMap;

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static size_t next_pow2(size_t x) {
    size_t p = 1;
    while (p < x) p <<= 1;
    return p;
}

static void hm_init(HashMap* hm, size_t initial_cap) {
    hm->cap = next_pow2(initial_cap < 16 ? 16 : initial_cap);
    hm->slots = (HashSlot*)calloc(hm->cap, sizeof(HashSlot));
    if (!hm->slots) { puts("hash alloc fail"); exit(1); }
    hm->size = 0;
    hm->tombs = 0;
}

static void hm_free(HashMap* hm) {
    free(hm->slots);
    hm->slots = NULL;
    hm->cap = hm->size = hm->tombs = 0;
}

static void hm_rehash(HashMap* hm, size_t new_cap) {
    HashSlot* old = hm->slots;
    size_t oldcap = hm->cap;

    hm_init(hm, new_cap);

    for (size_t i = 0; i < oldcap; i++) {
        if (old[i].state == SLOT_FILLED) {
            uint32_t h = hash_u32((uint32_t)old[i].key);
            size_t mask = hm->cap - 1;
            for (size_t step = 0;; step++) {
                size_t idx = (h + step) & mask;
                if (hm->slots[idx].state != SLOT_FILLED) {
                    hm->slots[idx] = old[i];
                    hm->slots[idx].state = SLOT_FILLED;
                    hm->size++;
                    break;
                }
            }
        }
    }
    free(old);
}

static void hm_maybe_grow(HashMap* hm) {
    double used = (double)(hm->size + hm->tombs) / (double)hm->cap;
    if (used > 0.75) hm_rehash(hm, hm->cap * 2);
    else if (hm->tombs > hm->size && hm->tombs > 32) hm_rehash(hm, hm->cap);
}

static int hm_put(HashMap* hm, int key, size_t value) {
    hm_maybe_grow(hm);
    uint32_t h = hash_u32((uint32_t)key);
    size_t mask = hm->cap - 1;
    size_t first_tomb = (size_t)-1;

    for (size_t step = 0;; step++) {
        size_t idx = (h + step) & mask;
        HashSlot* s = &hm->slots[idx];

        if (s->state == SLOT_EMPTY) {
            if (first_tomb != (size_t)-1) s = &hm->slots[first_tomb];
            s->key = key;
            s->value = value;
            s->state = SLOT_FILLED;
            hm->size++;
            if (first_tomb != (size_t)-1) hm->tombs--;
            return 1;
        }
        if (s->state == SLOT_TOMB) {
            if (first_tomb == (size_t)-1) first_tomb = idx;
            continue;
        }
        if (s->state == SLOT_FILLED && s->key == key) {
            s->value = value;
            return 1;
        }
    }
}

static int hm_get(const HashMap* hm, int key, size_t* out_value) {
    if (!hm->slots || hm->cap == 0) return 0;
    uint32_t h = hash_u32((uint32_t)key);
    size_t mask = hm->cap - 1;

    for (size_t step = 0; step < hm->cap; step++) {
        size_t idx = (h + step) & mask;
        const HashSlot* s = &hm->slots[idx];
        if (s->state == SLOT_EMPTY) return 0;
        if (s->state == SLOT_FILLED && s->key == key) {
            *out_value = s->value;
            return 1;
        }
    }
    return 0;
}

static int hm_remove(HashMap* hm, int key) {
    if (!hm->slots || hm->cap == 0) return 0;
    uint32_t h = hash_u32((uint32_t)key);
    size_t mask = hm->cap - 1;

    for (size_t step = 0; step < hm->cap; step++) {
        size_t idx = (h + step) & mask;
        HashSlot* s = &hm->slots[idx];

        if (s->state == SLOT_EMPTY) return 0;
        if (s->state == SLOT_FILLED && s->key == key) {
            s->state = SLOT_TOMB;
            hm->size--;
            hm->tombs++;
            hm_maybe_grow(hm);
            return 1;
        }
    }
    return 0;
}

/* ===========================
   MovieDB
=========================== */
typedef struct {
    Movie* data;
    size_t size;
    size_t cap;
    int next_id;
    HashMap index;
} MovieDB;

static void db_init(MovieDB* db) {
    db->data = NULL;
    db->size = 0;
    db->cap = 0;
    db->next_id = 1;
    hm_init(&db->index, 16);
}

static void db_free(MovieDB* db) {
    free(db->data);
    db->data = NULL;
    db->size = db->cap = 0;
    db->next_id = 1;
    hm_free(&db->index);
}

static void db_ensure(MovieDB* db, size_t need) {
    if (db->cap >= need) return;
    size_t newcap = (db->cap == 0) ? 8 : db->cap * 2;
    while (newcap < need) newcap *= 2;

    Movie* p = (Movie*)realloc(db->data, newcap * sizeof(Movie));
    if (!p) { puts("메모리 할당 실패!"); exit(1); }
    db->data = p;
    db->cap = newcap;
}

static void db_add(MovieDB* db, Movie m) {
    db_ensure(db, db->size + 1);
    db->data[db->size] = m;
    hm_put(&db->index, m.id, db->size);
    db->size++;
}

static Movie* db_find_by_id(MovieDB* db, int id) {
    size_t idx;
    if (!hm_get(&db->index, id, &idx)) return NULL;
    if (idx >= db->size) return NULL;
    return &db->data[idx];
}

/* swap-delete: 마지막 원소를 삭제 자리로 옮기고 size-- */
static int db_delete_by_id(MovieDB* db, int id) {
    size_t idx;
    if (!hm_get(&db->index, id, &idx)) return 0;

    size_t last = db->size - 1;
    hm_remove(&db->index, id);

    if (idx != last) {
        Movie moved = db->data[last];
        db->data[idx] = moved;
        hm_put(&db->index, moved.id, idx);
    }
    db->size--;
    return 1;
}

/* ===========================
   Input utils
=========================== */
static void trim_newline(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n') s[n - 1] = '\0';
}

static void read_line(const char* prompt, char* buf, size_t buflen) {
    if (prompt) printf("%s", prompt);
    if (!fgets(buf, (int)buflen, stdin)) { buf[0] = '\0'; return; }
    trim_newline(buf);
}

static int read_int(const char* prompt, int minv, int maxv) {
    char line[LINE_MAX];
    for (;;) {
        read_line(prompt, line, sizeof(line));
        if (line[0] == '\0') continue;
        char* end = NULL;
        long v = strtol(line, &end, 10);
        while (end && *end && isspace((unsigned char)*end)) end++;
        if (end && *end != '\0') { puts("숫자만 입력해주세요."); continue; }
        if (v < minv || v > maxv) { printf("범위(%d~%d)\n", minv, maxv); continue; }
        return (int)v;
    }
}

static double read_double(const char* prompt, double minv, double maxv) {
    char line[LINE_MAX];
    for (;;) {
        read_line(prompt, line, sizeof(line));
        if (line[0] == '\0') continue;
        char* end = NULL;
        double v = strtod(line, &end);
        while (end && *end && isspace((unsigned char)*end)) end++;
        if (end && *end != '\0') { puts("실수만 입력해주세요."); continue; }
        if (v < minv || v > maxv) { printf("범위(%.1f~%.1f)\n", minv, maxv); continue; }
        return v;
    }
}

static int contains_case_insensitive(const char* hay, const char* needle) {
    if (!hay || !needle) return 0;
    size_t hlen = strlen(hay), nlen = strlen(needle);
    if (nlen == 0) return 1;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = (char)tolower((unsigned char)hay[i + j]);
            char b = (char)tolower((unsigned char)needle[j]);
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* ===========================
   Pretty Console UI helpers
=========================== */

/* 화면을 깔끔히: VS CMD 콘솔에서 효과 있음 */
static void clear_screen(void) {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

static void pause_enter(void) {
    puts("\n(Enter를 누르면 계속)");
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

/* 제목/구분선 스타일 */
static void print_line(char ch, int n) {
    for (int i = 0; i < n; i++) putchar(ch);
    putchar('\n');
}

static void print_title(const char* t) {
    print_line('=', 60);
    printf("  %s\n", t);
    print_line('=', 60);
}

/* 한 영화 '카드' 형태 출력: 한글 정렬 문제(폭 계산) 없이 깔끔 */
static void print_movie_card(const Movie* m) {
    print_line('-', 60);
    printf(" ID       : %d\n", m->id);
    printf(" 제목     : %s\n", m->title);
    printf(" 장르     : %s\n", m->genre);
    printf(" 감독     : %s\n", m->director);
    printf(" 개봉년도 : %d\n", m->year);
    printf(" 러닝타임 : %d분\n", m->runtime);
    printf(" 평점     : %.1f\n", m->rating);
    print_line('-', 60);
}

/* 목록은 간단하게(번호/제목/감독 정도) + 상세 보기로 예쁘게 */
static void print_movie_compact(const Movie* m) {
    printf(" [%d] %s  |  %s  |  %.1f\n", m->id, m->title, m->director, m->rating);
}

/* ===========================
   Binary Save/Load
=========================== */
typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t next_id;
    uint64_t count;
} FileHeader;

#define MVDB_MAGIC 0x4D564442u

static void ui_save_bin(const MovieDB* db) {
    char path[LINE_MAX];
    read_line("저장 파일명(예: movies.dat): ", path, sizeof(path));
    if (path[0] == '\0') { puts("파일명이 비었습니다."); return; }

    FILE* fp = fopen(path, "wb");
    if (!fp) { puts("파일 열기 실패!"); return; }

    FileHeader hdr = { MVDB_MAGIC, 1, db->next_id, (uint64_t)db->size };

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) { puts("저장 실패(헤더)"); fclose(fp); return; }
    if (db->size > 0) {
        if (fwrite(db->data, sizeof(Movie), db->size, fp) != db->size) {
            puts("저장 실패(데이터)"); fclose(fp); return;
        }
    }
    fclose(fp);
    puts("✅ 저장 완료!");
}

static void ui_load_bin(MovieDB* db) {
    char path[LINE_MAX];
    read_line("불러올 파일명(예: movies.dat): ", path, sizeof(path));
    if (path[0] == '\0') { puts("파일명이 비었습니다."); return; }

    FILE* fp = fopen(path, "rb");
    if (!fp) { puts("파일 열기 실패!"); return; }

    FileHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) { puts("불러오기 실패(헤더)"); fclose(fp); return; }
    if (hdr.magic != MVDB_MAGIC || hdr.version != 1) { puts("파일 형식이 올바르지 않습니다."); fclose(fp); return; }

    db_free(db);
    db_init(db);

    db->next_id = (int)hdr.next_id;
    size_t count = (size_t)hdr.count;

    if (count > 0) {
        db_ensure(db, count);
        size_t nread = fread(db->data, sizeof(Movie), count, fp);
        if (nread != count) { puts("불러오기 실패(데이터 부족)"); fclose(fp); return; }
        db->size = count;

        for (size_t i = 0; i < db->size; i++) {
            hm_put(&db->index, db->data[i].id, i);
        }
    }
    fclose(fp);
    puts("✅ 불러오기 완료!");
}

/* ===========================
   UI Actions (pretty)
=========================== */
static void ui_add(MovieDB* db) {
    clear_screen();
    print_title("영화 추가");

    Movie m;
    memset(&m, 0, sizeof(m));
    m.id = db->next_id++;

    read_line("제목: ", m.title, sizeof(m.title));
    read_line("장르: ", m.genre, sizeof(m.genre));
    read_line("감독: ", m.director, sizeof(m.director));
    m.year = read_int("개봉년도(1900~2100): ", 1900, 2100);
    m.runtime = read_int("러닝타임(1~500분): ", 1, 500);
    m.rating = read_double("평점(0.0~10.0): ", 0.0, 10.0);

    db_add(db, m);

    puts("\n✅ 추가 완료!");
    print_movie_card(&m);
    pause_enter();
}

static void ui_list(MovieDB* db) {
    clear_screen();
    print_title("영화 목록");

    if (db->size == 0) {
        puts("등록된 영화가 없습니다.");
        pause_enter();
        return;
    }

    puts("ID | 제목 | 감독 | 평점");
    print_line('-', 60);
    for (size_t i = 0; i < db->size; i++) {
        print_movie_compact(&db->data[i]);
    }
    print_line('-', 60);
    printf("총 %zu개\n", db->size);

    puts("\n상세보기: ID 입력 (0=뒤로가기)");
    int id = read_int("ID: ", 0, 1000000000);
    if (id == 0) return;

    Movie* m = db_find_by_id(db, id);
    if (!m) {
        puts("해당 ID를 찾을 수 없습니다.");
        pause_enter();
        return;
    }

    clear_screen();
    print_title("영화 상세 정보");
    print_movie_card(m);
    pause_enter();
}

static void ui_search(MovieDB* db) {
    clear_screen();
    print_title("영화 검색");

    char key[LINE_MAX];
    read_line("키워드(제목/장르/감독): ", key, sizeof(key));
    if (key[0] == '\0') { puts("키워드가 비었습니다."); pause_enter(); return; }

    size_t found = 0;
    print_line('-', 60);

    for (size_t i = 0; i < db->size; i++) {
        Movie* m = &db->data[i];
        if (contains_case_insensitive(m->title, key) ||
            contains_case_insensitive(m->genre, key) ||
            contains_case_insensitive(m->director, key)) {
            print_movie_compact(m);
            found++;
        }
    }

    print_line('-', 60);
    if (found == 0) puts("검색 결과가 없습니다.");
    else printf("검색 결과 %zu개\n", found);

    pause_enter();
}

static void ui_edit(MovieDB* db) {
    clear_screen();
    print_title("영화 수정");

    if (db->size == 0) {
        puts("수정할 영화가 없습니다.");
        pause_enter();
        return;
    }

    int id = read_int("수정할 영화 ID: ", 1, 1000000000);
    Movie* m = db_find_by_id(db, id);
    if (!m) {
        puts("해당 ID를 찾을 수 없습니다.");
        pause_enter();
        return;
    }

    puts("\n현재 정보:");
    print_movie_card(m);

    puts("Enter를 누르면 기존 값을 유지합니다.\n");
    char buf[LINE_MAX];

    read_line("새 제목: ", buf, sizeof(buf));
    if (buf[0] != '\0') { strncpy(m->title, buf, sizeof(m->title) - 1); m->title[sizeof(m->title) - 1] = 0; }

    read_line("새 장르: ", buf, sizeof(buf));
    if (buf[0] != '\0') { strncpy(m->genre, buf, sizeof(m->genre) - 1); m->genre[sizeof(m->genre) - 1] = 0; }

    read_line("새 감독: ", buf, sizeof(buf));
    if (buf[0] != '\0') { strncpy(m->director, buf, sizeof(m->director) - 1); m->director[sizeof(m->director) - 1] = 0; }

    read_line("새 개봉년도(빈칸=유지): ", buf, sizeof(buf));
    if (buf[0] != '\0') {
        char* end = NULL; long t = strtol(buf, &end, 10);
        while (end && *end && isspace((unsigned char)*end)) end++;
        if (end && *end == '\0' && t >= 1900 && t <= 2100) m->year = (int)t;
        else puts("개봉년도 입력이 올바르지 않아 유지합니다.");
    }

    read_line("새 러닝타임(빈칸=유지): ", buf, sizeof(buf));
    if (buf[0] != '\0') {
        char* end = NULL; long t = strtol(buf, &end, 10);
        while (end && *end && isspace((unsigned char)*end)) end++;
        if (end && *end == '\0' && t >= 1 && t <= 500) m->runtime = (int)t;
        else puts("러닝타임 입력이 올바르지 않아 유지합니다.");
    }

    read_line("새 평점(빈칸=유지): ", buf, sizeof(buf));
    if (buf[0] != '\0') {
        char* end = NULL; double t = strtod(buf, &end);
        while (end && *end && isspace((unsigned char)*end)) end++;
        if (end && *end == '\0' && t >= 0.0 && t <= 10.0) m->rating = t;
        else puts("평점 입력이 올바르지 않아 유지합니다.");
    }

    puts("\n✅ 수정 완료!");
    print_movie_card(m);
    pause_enter();
}

static void ui_delete(MovieDB* db) {
    clear_screen();
    print_title("영화 삭제");

    if (db->size == 0) {
        puts("삭제할 영화가 없습니다.");
        pause_enter();
        return;
    }

    int id = read_int("삭제할 영화 ID: ", 1, 1000000000);
    Movie* m = db_find_by_id(db, id);
    if (!m) {
        puts("해당 ID를 찾을 수 없습니다.");
        pause_enter();
        return;
    }

    puts("\n삭제 대상:");
    print_movie_card(m);

    puts("정말 삭제할까요? (y/n): ");
    int c = getchar();
    while (c != '\n') { int t = getchar(); if (t == '\n' || t == EOF) break; }

    if (!(c == 'y' || c == 'Y')) {
        puts("삭제 취소.");
        pause_enter();
        return;
    }

    if (db_delete_by_id(db, id)) puts("✅ 삭제 완료!");
    else puts("삭제 실패(내부 오류)");
    pause_enter();
}

static void ui_save(MovieDB* db) {
    clear_screen();
    print_title("저장");
    ui_save_bin(db);
    pause_enter();
}

static void ui_load(MovieDB* db) {
    clear_screen();
    print_title("불러오기");
    ui_load_bin(db);
    pause_enter();
}

/* ===========================
   Menu
=========================== */
static void print_menu(MovieDB* db) {
    clear_screen();
    print_title("MOVIE MANAGER");
    printf("  현재 등록된 영화: %zu개\n", db->size);
    print_line('-', 60);
    puts("  1) 영화 추가");
    puts("  2) 영화 목록/상세보기");
    puts("  3) 영화 검색");
    puts("  4) 영화 수정");
    puts("  5) 영화 삭제");
    puts("  6) 파일 저장");
    puts("  7) 파일 불러오기");
    puts("  8) 종료");
    print_line('-', 60);
}

int main(void) {
#ifdef _WIN32
    /* VS 콘솔에서 한글이 ?로 나오는 문제 완화 */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    MovieDB db;
    db_init(&db);

    for (;;) {
        print_menu(&db);
        int choice = read_int("메뉴 선택(1~8): ", 1, 8);

        switch (choice) {
        case 1: ui_add(&db); break;
        case 2: ui_list(&db); break;
        case 3: ui_search(&db); break;
        case 4: ui_edit(&db); break;
        case 5: ui_delete(&db); break;
        case 6: ui_save(&db); break;
        case 7: ui_load(&db); break;
        case 8:
            db_free(&db);
            clear_screen();
            print_title("종료");
            puts("프로그램을 종료합니다.");
            return 0;
        }
    }
}