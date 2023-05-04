#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>

/* The idea behind this implementation
 * is minimizing I/O for very large files, where page loads complexity
 * should be ~ O(log n).
 *
 * If we do reading and tokenization of the whole file, we will need O(n)
 * block reads and the only benefit will be just O(log n) for matching the key.
 *
 * Other advantage of this algorithm is that there is
 * no users space memory allocation and no memory copy.
 *
 * For small files performance probably could be equal or worse, so best universal
 * solution is to choose between two algorithm depending on file size. This is
 * beyond purpose of this small program, so I'm leaving measuring and tuning
 * performance for now.
 * There much room for optimization and
 * simplifying the code (e.g using binary_search_rightmost)
 *
 *  Algorithm itself:
 *  1. mmap the file (same idea could be done with file seeks, but much harder)
 *  2. find the middle (binary search) and select the row where middle is within
 *     the row - search left and right boundaries of the row
 *  3. repeat binary search until we match key and first column of row
 *  4. if we find it, check for adjacent regions for the same key. Since we
 *     have ordered file, they could be only on our left or right sides
 */


typedef struct {
    size_t len;
    char  *data;
} _str_t;

/*
 * Compare two words
 * @param word1 fist word to compare
 * @param word2 second word to compare
 * @return less than 0 if word1 is alphabetically smaller than word2,
 * greater than zero if word1 alphabetically smaller than word2 and zero if they
 * were the same words
 */
char compare_words(_str_t *word1, _str_t *word2)
{  
    size_t n;
    int    m, r;

#if 0
    printf("compare: [%.*s]  [%.*s]\n",
        (int)word1->len, word1->data, (int)word2->len, word2->data);
#endif

    if(word1->len <= word2->len) {
        n = word1->len;
        r = -1;
    } else {
        n = word1->len;
        r = 1;
    }

    m = memcmp(word1->data, word2->data, n);
 
    if(m || word1->len == word2->len) {
        return m;
    }

    return r;
}

/*
 * Find the left neighbor of row pointed by key_column and row_end
 * @param buf whole file used as limits
 * @param key_column column that has they same key as rows we want to find here
 * @return string containing left adjacent rows with same keys as
 * key_column
 */

char *find_left_neighbor(_str_t *buf, _str_t *key_column)
{
    _str_t  row;
    char   *p, *temp, *comma = NULL;
    size_t  current_row_len;

    /* skip trailing CR LF's, if any */
    for(p = key_column->data - 1; p >= buf->data; p--) {
        if(*p != '\n' && *p != '\r') {
            break;
        }
    }

    temp = p; // remember end of left adjacent row

    /* find next CR LF on the left - its the begging of row */
    for(; p > buf->data; p--) {
        /* mark last comma if any  */
        if(*p == ',') {
            comma = p;
        }

        if(*p == '\n' || *p == '\r') {
            p++;
            break;
        }
    }

    if(comma) {
        current_row_len = comma - p;
    } else {
        current_row_len = temp - p + 1;
    }

    if(key_column->len == current_row_len &&
            memcmp(key_column->data, p, current_row_len) == 0) {

        return p;
    }

    return NULL;
}

/*
 * Find the right neighbor of row pointed by key_column and row_end
 * @param buf whole file used as limits
 * @param key_column column that has they same key as rows we want to find here
 * @row_end points the end of row containing key_column
 * @return string containing right adjacent rows with same keys as
 * key_column
 */
char *find_right_neighbor(_str_t *buf, _str_t *key_column, char *row_end)
{
    char   *p, *temp, *comma = NULL;
    char   *row_begin;
    size_t  current_row_len;

    p =  row_end;

    /* skip leading CR LF's, if any*/
    for(; p < buf->data + buf->len; p++) {
        if(*p != '\n' && *p != '\r') {
            break;
        }
    }

    row_begin = p;

    /* find fist (key) column and end of row */
    for(; p < buf->data + buf->len; p++) {
        if(*p == ',' && comma == NULL) {
           comma = p;
        }
        /* we found beginning of next row */
        if(*p == '\n' || *p == '\r') {
            p++;
            break;
        }
    }

    if(comma) {
        current_row_len = comma - row_begin;
    } else {
        current_row_len = p - row_begin - 1;
    }

    if(key_column->len == current_row_len &&
            memcmp(key_column->data, row_begin, key_column->len) == 0) {
        return p;
    }

    return NULL;
}


/*
 * Search for adjacent rows, with same key as key_column
 * @param buf whole file used as limits
 * @param key_column column that has they same key as rows we want to find here
 * @param row_end points the end of row containing key_column
 * @return string containing left and right adjacent rows with same keys as
 * key_column
 */
_str_t find_left_right_neighbors(_str_t *buf, _str_t *key_column, char *row_end)
{   
    _str_t  result, temp_key_column;
    char *left;
    char *right;
    
    result.data = key_column->data;
    result.len = row_end - key_column->data;

    temp_key_column = *key_column;

    left = temp_key_column.data;
    do {
        left = find_left_neighbor(buf, &temp_key_column);

        if(left) {
            result.data = left;
            result.len =  row_end - left;
        }

        temp_key_column.data = left;

    } while(left);

    temp_key_column = *key_column;

    do {
        right = find_right_neighbor(buf, &temp_key_column, row_end);

        if(right) {
            result.len = right - result.data;
        }

        row_end = right;

    } while(right);

    return result;
}

/*
 *  Finds a single row pointed by mid
 *  @param left points left margin
 *  @parm right points right margin
 *  @parm mid points somewhere in the middle of the row
 *  @param row_end used for return value pointing to of end found row
 *  @return matching first (key) column of the row
 */
_str_t find_row(char *left, char *right, char *mid, char **row_end)
{
    char  *p;
    bool   comma_found = false;
    _str_t result;

    /* find beginning of row */
    for(p = mid; p > left; p--) {
        if(*p == '\n' || *p == '\r') {
            p++;
            break;
        }
    }

    /* we found beginning of row, save it */
    result.data = p;

    /* find fist (key) column and end of row */
    for(; p < right; p++) {
        if(*p == ',' && !comma_found) {
           comma_found = true;
           result.len = p - result.data;
        }
        /* we found beginning of next row */
        if(*p == '\n' || *p == '\r') {
            break;
        }
    }

    *row_end = p;

    if(!comma_found) {
        result.len = p - result.data;
    }
    
    return result;
}
/*
 *  Find matching rows, where first column is equal to key
 *  @param buf buffer containing the whole csv file
 *  @param key key which we use to search
 *  @return matching rows
 */
_str_t find_match(_str_t *buf, _str_t *key)
{
    char  *p, *row_end;
    char  *left, *right, *mid; 
    int    result;
    _str_t key_column, row, not_found = {0, NULL};

    left = buf->data;
    right = buf->data + buf->len;

    while (left <= right) {
        mid = (right - left) / 2 + left;
        
        key_column = find_row(left, right, mid, &row_end);
        
        result = compare_words(&key_column, key);

        if (result < 0) {
            left = row_end + 1; // next word on right
        } else if (result > 0) {
            right = key_column.data; // left margin is current word
        } else {
            return find_left_right_neighbors(buf, &key_column, row_end);
        }       
    }

    return not_found;
}

int main(int argc, char **argv)
{
    int     fd;
    struct  stat st;
    _str_t  buf, key, result;
    
    if(argc < 4 || strcmp(argv[1], "--key")) {
        fprintf(stderr, "Usage: %s --key key filename\n", argv[0]);
        exit(EXIT_FAILURE);
    }
   
    if((fd = open(argv[3], O_RDONLY)) == -1) {
        perror("filed to open");
        exit(EXIT_FAILURE);
    }

    if(fstat(fd, &st) < 0) {
        perror("filed to stat");
        close(fd);
        exit(EXIT_FAILURE);
    }

    buf.len = st.st_size;
    
    if((buf.data = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0))
            == MAP_FAILED) {
        perror("filed to mmap");
        close(fd);
        exit(EXIT_FAILURE);
    }

    /* tell the kernel that we are going to do random reads */
    if(madvise(buf.data, buf.len, MADV_RANDOM) ) {
        perror("filed to madvise");
    }

    key.len =  strlen(argv[2]);
    key.data = argv[2];

    result = find_match(&buf, &key);

    if(result.data) {
        if(write(STDOUT_FILENO, result.data, result.len) < 0) {
            perror("filed to write to stdout");
        }
    }

    if(munmap(buf.data, st.st_size) < 0) {
        perror("filed to munmap");
    }

    close(fd);

    exit(EXIT_SUCCESS);
}
