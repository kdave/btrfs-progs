/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "crypto/crc32c.h"
#include "image/sanitize.h"
#include "kernel-shared/extent_io.h"

/*
 * Reverse CRC-32C table
 */
static const u32 crc32c_rev_table[256] = {
	0x00000000L,0x05EC76F1L,0x0BD8EDE2L,0x0E349B13L,
	0x17B1DBC4L,0x125DAD35L,0x1C693626L,0x198540D7L,
	0x2F63B788L,0x2A8FC179L,0x24BB5A6AL,0x21572C9BL,
	0x38D26C4CL,0x3D3E1ABDL,0x330A81AEL,0x36E6F75FL,
	0x5EC76F10L,0x5B2B19E1L,0x551F82F2L,0x50F3F403L,
	0x4976B4D4L,0x4C9AC225L,0x42AE5936L,0x47422FC7L,
	0x71A4D898L,0x7448AE69L,0x7A7C357AL,0x7F90438BL,
	0x6615035CL,0x63F975ADL,0x6DCDEEBEL,0x6821984FL,
	0xBD8EDE20L,0xB862A8D1L,0xB65633C2L,0xB3BA4533L,
	0xAA3F05E4L,0xAFD37315L,0xA1E7E806L,0xA40B9EF7L,
	0x92ED69A8L,0x97011F59L,0x9935844AL,0x9CD9F2BBL,
	0x855CB26CL,0x80B0C49DL,0x8E845F8EL,0x8B68297FL,
	0xE349B130L,0xE6A5C7C1L,0xE8915CD2L,0xED7D2A23L,
	0xF4F86AF4L,0xF1141C05L,0xFF208716L,0xFACCF1E7L,
	0xCC2A06B8L,0xC9C67049L,0xC7F2EB5AL,0xC21E9DABL,
	0xDB9BDD7CL,0xDE77AB8DL,0xD043309EL,0xD5AF466FL,
	0x7EF1CAB1L,0x7B1DBC40L,0x75292753L,0x70C551A2L,
	0x69401175L,0x6CAC6784L,0x6298FC97L,0x67748A66L,
	0x51927D39L,0x547E0BC8L,0x5A4A90DBL,0x5FA6E62AL,
	0x4623A6FDL,0x43CFD00CL,0x4DFB4B1FL,0x48173DEEL,
	0x2036A5A1L,0x25DAD350L,0x2BEE4843L,0x2E023EB2L,
	0x37877E65L,0x326B0894L,0x3C5F9387L,0x39B3E576L,
	0x0F551229L,0x0AB964D8L,0x048DFFCBL,0x0161893AL,
	0x18E4C9EDL,0x1D08BF1CL,0x133C240FL,0x16D052FEL,
	0xC37F1491L,0xC6936260L,0xC8A7F973L,0xCD4B8F82L,
	0xD4CECF55L,0xD122B9A4L,0xDF1622B7L,0xDAFA5446L,
	0xEC1CA319L,0xE9F0D5E8L,0xE7C44EFBL,0xE228380AL,
	0xFBAD78DDL,0xFE410E2CL,0xF075953FL,0xF599E3CEL,
	0x9DB87B81L,0x98540D70L,0x96609663L,0x938CE092L,
	0x8A09A045L,0x8FE5D6B4L,0x81D14DA7L,0x843D3B56L,
	0xB2DBCC09L,0xB737BAF8L,0xB90321EBL,0xBCEF571AL,
	0xA56A17CDL,0xA086613CL,0xAEB2FA2FL,0xAB5E8CDEL,
	0xFDE39562L,0xF80FE393L,0xF63B7880L,0xF3D70E71L,
	0xEA524EA6L,0xEFBE3857L,0xE18AA344L,0xE466D5B5L,
	0xD28022EAL,0xD76C541BL,0xD958CF08L,0xDCB4B9F9L,
	0xC531F92EL,0xC0DD8FDFL,0xCEE914CCL,0xCB05623DL,
	0xA324FA72L,0xA6C88C83L,0xA8FC1790L,0xAD106161L,
	0xB49521B6L,0xB1795747L,0xBF4DCC54L,0xBAA1BAA5L,
	0x8C474DFAL,0x89AB3B0BL,0x879FA018L,0x8273D6E9L,
	0x9BF6963EL,0x9E1AE0CFL,0x902E7BDCL,0x95C20D2DL,
	0x406D4B42L,0x45813DB3L,0x4BB5A6A0L,0x4E59D051L,
	0x57DC9086L,0x5230E677L,0x5C047D64L,0x59E80B95L,
	0x6F0EFCCAL,0x6AE28A3BL,0x64D61128L,0x613A67D9L,
	0x78BF270EL,0x7D5351FFL,0x7367CAECL,0x768BBC1DL,
	0x1EAA2452L,0x1B4652A3L,0x1572C9B0L,0x109EBF41L,
	0x091BFF96L,0x0CF78967L,0x02C31274L,0x072F6485L,
	0x31C993DAL,0x3425E52BL,0x3A117E38L,0x3FFD08C9L,
	0x2678481EL,0x23943EEFL,0x2DA0A5FCL,0x284CD30DL,
	0x83125FD3L,0x86FE2922L,0x88CAB231L,0x8D26C4C0L,
	0x94A38417L,0x914FF2E6L,0x9F7B69F5L,0x9A971F04L,
	0xAC71E85BL,0xA99D9EAAL,0xA7A905B9L,0xA2457348L,
	0xBBC0339FL,0xBE2C456EL,0xB018DE7DL,0xB5F4A88CL,
	0xDDD530C3L,0xD8394632L,0xD60DDD21L,0xD3E1ABD0L,
	0xCA64EB07L,0xCF889DF6L,0xC1BC06E5L,0xC4507014L,
	0xF2B6874BL,0xF75AF1BAL,0xF96E6AA9L,0xFC821C58L,
	0xE5075C8FL,0xE0EB2A7EL,0xEEDFB16DL,0xEB33C79CL,
	0x3E9C81F3L,0x3B70F702L,0x35446C11L,0x30A81AE0L,
	0x292D5A37L,0x2CC12CC6L,0x22F5B7D5L,0x2719C124L,
	0x11FF367BL,0x1413408AL,0x1A27DB99L,0x1FCBAD68L,
	0x064EEDBFL,0x03A29B4EL,0x0D96005DL,0x087A76ACL,
	0x605BEEE3L,0x65B79812L,0x6B830301L,0x6E6F75F0L,
	0x77EA3527L,0x720643D6L,0x7C32D8C5L,0x79DEAE34L,
	0x4F38596BL,0x4AD42F9AL,0x44E0B489L,0x410CC278L,
	0x588982AFL,0x5D65F45EL,0x53516F4DL,0x56BD19BCL
};

/*
 * Calculate a 4-byte suffix to match desired CRC32C
 *
 * @current_crc: CRC32C checksum of all bytes before the suffix
 * @desired_crc: the checksum that we want to get after adding the suffix
 *
 * Outputs: @suffix: pointer to where the suffix will be written (4-bytes)
 */
static void find_collision_calc_suffix(unsigned long current_crc,
				       unsigned long desired_crc,
				       char *suffix)
{
	int i;

	for(i = 3; i >= 0; i--) {
		desired_crc = (desired_crc << 8)
			    ^ crc32c_rev_table[desired_crc >> 24 & 0xFF]
			    ^ ((current_crc >> i * 8) & 0xFF);
	}
	for (i = 0; i < 4; i++)
		suffix[i] = (desired_crc >> i * 8) & 0xFF;
}

/*
 * Check if suffix is valid according to our file name conventions
 */
static int find_collision_is_suffix_valid(const char *suffix)
{
	int i;
	char c;

	for (i = 0; i < 4; i++) {
		c = suffix[i];
		if (c < ' ' || c > 126 || c == '/')
			return 0;
	}
	return 1;
}

static int find_collision_reverse_crc32c(struct name *val, u32 name_len)
{
	unsigned long checksum;
	unsigned long current_checksum;
	int found = 0;
	int i;

	/* There are no same length collisions of 4 or less bytes */
	if (name_len <= 4)
		return 0;
	checksum = crc32c(~1, val->val, name_len);
	name_len -= 4;
	memset(val->sub, ' ', name_len);
	i = 0;
	while (1) {
		current_checksum = crc32c(~1, val->sub, name_len);
		find_collision_calc_suffix(current_checksum,
					   checksum,
					   val->sub + name_len);
		if (find_collision_is_suffix_valid(val->sub + name_len) &&
		    memcmp(val->sub, val->val, val->len)) {
			found = 1;
			break;
		}

		if (val->sub[i] == 126) {
			do {
				i++;
				if (i >= name_len)
					break;
			} while (val->sub[i] == 126);

			if (i >= name_len)
				break;
			val->sub[i]++;
			if (val->sub[i] == '/')
				val->sub[i]++;
			memset(val->sub, ' ', i);
			i = 0;
			continue;
		} else {
			val->sub[i]++;
			if (val->sub[i] == '/')
				val->sub[i]++;
		}
	}
	return found;
}

static void tree_insert(struct rb_root *root, struct rb_node *ins,
			int (*cmp)(struct rb_node *a, struct rb_node *b,
				   int fuzz))
{
	struct rb_node ** p = &root->rb_node;
	struct rb_node * parent = NULL;
	int dir;

	while(*p) {
		parent = *p;

		dir = cmp(*p, ins, 1);
		if (dir < 0)
			p = &(*p)->rb_left;
		else if (dir > 0)
			p = &(*p)->rb_right;
		else
			BUG();
	}

	rb_link_node(ins, parent, p);
	rb_insert_color(ins, root);
}

static struct rb_node *tree_search(struct rb_root *root,
				   struct rb_node *search,
				   int (*cmp)(struct rb_node *a,
					      struct rb_node *b, int fuzz),
				   int fuzz)
{
	struct rb_node *n = root->rb_node;
	int dir;

	while (n) {
		dir = cmp(n, search, fuzz);
		if (dir < 0)
			n = n->rb_left;
		else if (dir > 0)
			n = n->rb_right;
		else
			return n;
	}

	return NULL;
}

static int name_cmp(struct rb_node *a, struct rb_node *b, int fuzz)
{
	struct name *entry = rb_entry(a, struct name, n);
	struct name *ins = rb_entry(b, struct name, n);
	u32 len;

	len = min(ins->len, entry->len);
	return memcmp(ins->val, entry->val, len);
}

static char *find_collision(struct rb_root *name_tree, char *name,
			    u32 name_len)
{
	struct name *val;
	struct rb_node *entry;
	struct name tmp;
	int found;
	int i;

	tmp.val = name;
	tmp.len = name_len;
	entry = tree_search(name_tree, &tmp.n, name_cmp, 0);
	if (entry) {
		val = rb_entry(entry, struct name, n);
		free(name);
		return val->sub;
	}

	val = malloc(sizeof(struct name));
	if (!val) {
		error("cannot sanitize name, not enough memory");
		free(name);
		return NULL;
	}

	memset(val, 0, sizeof(*val));

	val->val = name;
	val->len = name_len;
	val->sub = malloc(name_len);
	if (!val->sub) {
		error("cannot sanitize name, not enough memory");
		free(val);
		free(name);
		return NULL;
	}

	found = find_collision_reverse_crc32c(val, name_len);

	if (!found) {
		warning(
"cannot find a hash collision for '%.*s', generating garbage, it won't match indexes",
			val->len, val->val);
		for (i = 0; i < name_len; i++) {
			char c = rand_range(94) + 33;

			if (c == '/')
				c++;
			val->sub[i] = c;
		}
	}

	tree_insert(name_tree, &val->n, name_cmp);
	return val->sub;
}

static char *generate_garbage(u32 name_len)
{
	char *buf = malloc(name_len);
	int i;

	if (!buf)
		return NULL;

	for (i = 0; i < name_len; i++) {
		char c = rand_range(94) + 33;

		if (c == '/')
			c++;
		buf[i] = c;
	}

	return buf;
}

static void sanitize_dir_item(enum sanitize_mode sanitize,
		struct rb_root *name_tree, struct extent_buffer *eb, int slot)
{
	struct btrfs_dir_item *dir_item;
	char *buf;
	char *garbage;
	unsigned long name_ptr;
	u32 total_len;
	u32 cur = 0;
	u32 this_len;
	u32 name_len;
	int free_garbage = (sanitize == SANITIZE_NAMES);

	dir_item = btrfs_item_ptr(eb, slot, struct btrfs_dir_item);
	total_len = btrfs_item_size(eb, slot);
	while (cur < total_len) {
		this_len = sizeof(*dir_item) +
			btrfs_dir_name_len(eb, dir_item) +
			btrfs_dir_data_len(eb, dir_item);
		name_ptr = (unsigned long)(dir_item + 1);
		name_len = btrfs_dir_name_len(eb, dir_item);

		if (sanitize == SANITIZE_COLLISIONS) {
			buf = malloc(name_len);
			if (!buf) {
				error("cannot sanitize name, not enough memory");
				return;
			}
			read_extent_buffer(eb, buf, name_ptr, name_len);
			garbage = find_collision(name_tree, buf, name_len);
		} else {
			garbage = generate_garbage(name_len);
		}
		if (!garbage) {
			error("cannot sanitize name, not enough memory");
			return;
		}
		write_extent_buffer(eb, garbage, name_ptr, name_len);
		cur += this_len;
		dir_item = (struct btrfs_dir_item *)((char *)dir_item +
						     this_len);
		if (free_garbage)
			free(garbage);
	}
}

static void sanitize_inode_ref(enum sanitize_mode sanitize,
		struct rb_root *name_tree, struct extent_buffer *eb, int slot,
		int ext)
{
	struct btrfs_inode_extref *extref;
	struct btrfs_inode_ref *ref;
	char *garbage, *buf;
	unsigned long ptr;
	unsigned long name_ptr;
	u32 item_size;
	u32 cur_offset = 0;
	int len;
	int free_garbage = (sanitize == SANITIZE_NAMES);

	item_size = btrfs_item_size(eb, slot);
	ptr = btrfs_item_ptr_offset(eb, slot);
	while (cur_offset < item_size) {
		if (ext) {
			extref = (struct btrfs_inode_extref *)(ptr +
							       cur_offset);
			name_ptr = (unsigned long)(&extref->name);
			len = btrfs_inode_extref_name_len(eb, extref);
			cur_offset += sizeof(*extref);
		} else {
			ref = (struct btrfs_inode_ref *)(ptr + cur_offset);
			len = btrfs_inode_ref_name_len(eb, ref);
			name_ptr = (unsigned long)(ref + 1);
			cur_offset += sizeof(*ref);
		}
		cur_offset += len;

		if (sanitize == SANITIZE_COLLISIONS) {
			buf = malloc(len);
			if (!buf) {
				error("cannot sanitize name, not enough memory");
				return;
			}
			read_extent_buffer(eb, buf, name_ptr, len);
			garbage = find_collision(name_tree, buf, len);
		} else {
			garbage = generate_garbage(len);
		}

		if (!garbage) {
			error("cannot sanitize name, not enough memory");
			return;
		}
		write_extent_buffer(eb, garbage, name_ptr, len);
		if (free_garbage)
			free(garbage);
	}
}

static void sanitize_xattr(struct extent_buffer *eb, int slot)
{
	struct btrfs_dir_item *dir_item;
	unsigned long data_ptr;
	u32 data_len;

	dir_item = btrfs_item_ptr(eb, slot, struct btrfs_dir_item);
	data_len = btrfs_dir_data_len(eb, dir_item);

	data_ptr = (unsigned long)((char *)(dir_item + 1) +
				   btrfs_dir_name_len(eb, dir_item));
	memset_extent_buffer(eb, 0, data_ptr, data_len);
}

static struct extent_buffer *alloc_dummy_eb(u64 bytenr, u32 size)
{
	struct extent_buffer *eb;

	eb = calloc(1, sizeof(struct extent_buffer) + size);
	if (!eb)
		return NULL;

	eb->start = bytenr;
	eb->len = size;
	return eb;
}

void sanitize_name(enum sanitize_mode sanitize, struct rb_root *name_tree,
		u8 *dst, struct extent_buffer *src, struct btrfs_key *key,
		int slot)
{
	struct extent_buffer *eb;

	eb = alloc_dummy_eb(src->start, src->len);
	if (!eb) {
		error("cannot sanitize name, not enough memory");
		return;
	}

	memcpy(eb->data, src->data, src->len);

	switch (key->type) {
	case BTRFS_DIR_ITEM_KEY:
	case BTRFS_DIR_INDEX_KEY:
		sanitize_dir_item(sanitize, name_tree, eb, slot);
		break;
	case BTRFS_INODE_REF_KEY:
		sanitize_inode_ref(sanitize, name_tree, eb, slot, 0);
		break;
	case BTRFS_INODE_EXTREF_KEY:
		sanitize_inode_ref(sanitize, name_tree, eb, slot, 1);
		break;
	case BTRFS_XATTR_ITEM_KEY:
		sanitize_xattr(eb, slot);
		break;
	default:
		break;
	}

	memcpy(dst, eb->data, eb->len);
	free(eb);
}

