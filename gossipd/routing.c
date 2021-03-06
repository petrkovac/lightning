#include "routing.h"
#include <arpa/inet.h>
#include <bitcoin/block.h>
#include <bitcoin/script.h>
#include <ccan/array_size/array_size.h>
#include <ccan/endian/endian.h>
#include <ccan/structeq/structeq.h>
#include <ccan/tal/str/str.h>
#include <common/features.h>
#include <common/pseudorand.h>
#include <common/status.h>
#include <common/type_to_string.h>
#include <common/wireaddr.h>
#include <inttypes.h>
#include <wire/gen_onion_wire.h>
#include <wire/gen_peer_wire.h>

#ifndef SUPERVERBOSE
#define SUPERVERBOSE(...)
#endif

/* 365.25 * 24 * 60 / 10 */
#define BLOCKS_PER_YEAR 52596

/* For overflow avoidance, we never deal with msatoshi > 40 bits. */
#define MAX_MSATOSHI (1ULL << 40)

/* Proportional fee must be less than 24 bits, so never overflows. */
#define MAX_PROPORTIONAL_FEE (1 << 24)

/* We've unpacked and checked its signatures, now we wait for master to tell
 * us the txout to check */
struct pending_cannouncement {
	/* Off routing_state->pending_cannouncement */
	struct list_node list;

	/* Unpacked fields here */
	struct short_channel_id short_channel_id;
	struct pubkey node_id_1;
	struct pubkey node_id_2;
	struct pubkey bitcoin_key_1;
	struct pubkey bitcoin_key_2;

	/* The raw bits */
	const u8 *announce;

	/* Deferred updates, if we received them while waiting for
	 * this (one for each direction) */
	const u8 *updates[2];

	/* Only ever replace with newer updates */
	u32 update_timestamps[2];
};

struct pending_node_announce {
	struct pubkey nodeid;
	u8 *node_announcement;
	u32 timestamp;
};

static const secp256k1_pubkey *
pending_node_announce_keyof(const struct pending_node_announce *a)
{
	return &a->nodeid.pubkey;
}

static bool pending_node_announce_eq(const struct pending_node_announce *pna,
				     const secp256k1_pubkey *key)
{
	return structeq(&pna->nodeid.pubkey, key);
}

HTABLE_DEFINE_TYPE(struct pending_node_announce, pending_node_announce_keyof,
		   node_map_hash_key, pending_node_announce_eq,
		   pending_node_map);

static struct node_map *empty_node_map(const tal_t *ctx)
{
	struct node_map *map = tal(ctx, struct node_map);
	node_map_init(map);
	tal_add_destructor(map, node_map_clear);
	return map;
}

struct routing_state *new_routing_state(const tal_t *ctx,
					const struct bitcoin_blkid *chain_hash,
					const struct pubkey *local_id,
					u32 prune_timeout)
{
	struct routing_state *rstate = tal(ctx, struct routing_state);
	rstate->nodes = empty_node_map(rstate);
	rstate->broadcasts = new_broadcast_state(rstate);
	rstate->chain_hash = *chain_hash;
	rstate->local_id = *local_id;
	rstate->prune_timeout = prune_timeout;
	list_head_init(&rstate->pending_cannouncement);
	uintmap_init(&rstate->chanmap);

	rstate->pending_node_map = tal(ctx, struct pending_node_map);
	pending_node_map_init(rstate->pending_node_map);

	return rstate;
}


const secp256k1_pubkey *node_map_keyof_node(const struct node *n)
{
	return &n->id.pubkey;
}

size_t node_map_hash_key(const secp256k1_pubkey *key)
{
	return siphash24(siphash_seed(), key, sizeof(*key));
}

bool node_map_node_eq(const struct node *n, const secp256k1_pubkey *key)
{
	return structeq(&n->id.pubkey, key);
}

static void destroy_node(struct node *node, struct routing_state *rstate)
{
	node_map_del(rstate->nodes, node);

	/* These remove themselves from the array. */
	while (tal_count(node->chans))
		tal_free(node->chans[0]);
}

struct node *get_node(struct routing_state *rstate, const struct pubkey *id)
{
	return node_map_get(rstate->nodes, &id->pubkey);
}

static struct node *new_node(struct routing_state *rstate,
			     const struct pubkey *id)
{
	struct node *n;

	assert(!get_node(rstate, id));

	n = tal(rstate, struct node);
	n->id = *id;
	n->chans = tal_arr(n, struct chan *, 0);
	n->alias = NULL;
	n->node_announcement = NULL;
	n->announcement_idx = 0;
	n->last_timestamp = -1;
	n->addresses = tal_arr(n, struct wireaddr, 0);
	node_map_add(rstate->nodes, n);
	tal_add_destructor2(n, destroy_node, rstate);

	return n;
}

static bool remove_channel_from_array(struct chan ***chans, struct chan *c)
{
	size_t i, n;

	n = tal_count(*chans);
	for (i = 0; i < n; i++) {
		if ((*chans)[i] != c)
			continue;
		n--;
		memmove(*chans + i, *chans + i + 1, sizeof(**chans) * (n - i));
		tal_resize(chans, n);
		return true;
	}
	return false;
}

static void destroy_chan(struct chan *chan, struct routing_state *rstate)
{
	if (!remove_channel_from_array(&chan->nodes[0]->chans, chan)
	    || !remove_channel_from_array(&chan->nodes[1]->chans, chan))
		/* FIXME! */
		abort();

	uintmap_del(&rstate->chanmap, chan->scid.u64);

	if (tal_count(chan->nodes[0]->chans) == 0)
		tal_free(chan->nodes[0]);
	if (tal_count(chan->nodes[1]->chans) == 0)
		tal_free(chan->nodes[1]);
}

static void init_half_chan(struct routing_state *rstate,
				 struct chan *chan,
				 int idx)
{
	struct half_chan *c = &chan->half[idx];

	c->channel_update = NULL;
	c->channel_update_msgidx = 0;
	c->unroutable_until = 0;
	c->active = false;
	c->flags = idx;
	/* We haven't seen channel_update: make it halfway to prune time,
	 * which should be older than any update we'd see. */
	c->last_timestamp = time_now().ts.tv_sec - rstate->prune_timeout/2;
}

struct chan *new_chan(struct routing_state *rstate,
		      const struct short_channel_id *scid,
		      const struct pubkey *id1,
		      const struct pubkey *id2)
{
	struct chan *chan = tal(rstate, struct chan);
	int n1idx = pubkey_idx(id1, id2);
	size_t n;
	struct node *n1, *n2;

	/* Create nodes on demand */
	n1 = get_node(rstate, id1);
	if (!n1)
		n1 = new_node(rstate, id1);
	n2 = get_node(rstate, id2);
	if (!n2)
		n2 = new_node(rstate, id2);

	chan->scid = *scid;
	chan->nodes[n1idx] = n1;
	chan->nodes[!n1idx] = n2;
	chan->txout_script = NULL;
	chan->channel_announcement = NULL;
	chan->channel_announce_msgidx = 0;
	chan->public = false;
	chan->satoshis = 0;

	n = tal_count(n2->chans);
	tal_resize(&n2->chans, n+1);
	n2->chans[n] = chan;
	n = tal_count(n1->chans);
	tal_resize(&n1->chans, n+1);
	n1->chans[n] = chan;

	/* Populate with (inactive) connections */
	init_half_chan(rstate, chan, n1idx);
	init_half_chan(rstate, chan, !n1idx);

	uintmap_add(&rstate->chanmap, scid->u64, chan);

	tal_add_destructor2(chan, destroy_chan, rstate);
	return chan;
}

/* Too big to reach, but don't overflow if added. */
#define INFINITE 0x3FFFFFFFFFFFFFFFULL

static void clear_bfg(struct node_map *nodes)
{
	struct node *n;
	struct node_map_iter it;

	for (n = node_map_first(nodes, &it); n; n = node_map_next(nodes, &it)) {
		size_t i;
		for (i = 0; i < ARRAY_SIZE(n->bfg); i++) {
			n->bfg[i].total = INFINITE;
			n->bfg[i].risk = 0;
		}
	}
}

static u64 connection_fee(const struct half_chan *c, u64 msatoshi)
{
	u64 fee;

	assert(msatoshi < MAX_MSATOSHI);
	assert(c->proportional_fee < MAX_PROPORTIONAL_FEE);

	fee = (c->proportional_fee * msatoshi) / 1000000;
	/* This can't overflow: c->base_fee is a u32 */
	return c->base_fee + fee;
}

/* Risk of passing through this channel.  We insert a tiny constant here
 * in order to prefer shorter routes, all things equal. */
static u64 risk_fee(u64 amount, u32 delay, double riskfactor)
{
	return 1 + amount * delay * riskfactor;
}

/* We track totals, rather than costs.  That's because the fee depends
 * on the current amount passing through. */
static void bfg_one_edge(struct node *node,
			 struct chan *chan, int idx,
			 double riskfactor,
			 double fuzz, const struct siphash_seed *base_seed)
{
	size_t h;
	double fee_scale = 1.0;
	const struct half_chan *c = &chan->half[idx];

	if (fuzz != 0.0) {
		u64 h =	siphash24(base_seed, &chan->scid, sizeof(chan->scid));

		/* Scale fees for this channel */
		/* rand = (h / UINT64_MAX)  random number between 0.0 -> 1.0
		 * 2*fuzz*rand              random number between 0.0 -> 2*fuzz
		 * 2*fuzz*rand - fuzz       random number between -fuzz -> +fuzz
		 */
		fee_scale = 1.0 + (2.0 * fuzz * h / UINT64_MAX) - fuzz;
	}

	for (h = 0; h < ROUTING_MAX_HOPS; h++) {
		struct node *src;
		/* FIXME: Bias against smaller channels. */
		u64 fee;
		u64 risk;

		if (node->bfg[h].total == INFINITE)
			continue;

		fee = connection_fee(c, node->bfg[h].total) * fee_scale;
		risk = node->bfg[h].risk + risk_fee(node->bfg[h].total + fee,
						    c->delay, riskfactor);

		if (node->bfg[h].total + fee + risk >= MAX_MSATOSHI) {
			SUPERVERBOSE("...extreme %"PRIu64
				     " + fee %"PRIu64
				     " + risk %"PRIu64" ignored",
				     node->bfg[h].total, fee, risk);
			continue;
		}

		/* nodes[0] is src for connections[0] */
		src = chan->nodes[idx];
		if (node->bfg[h].total + fee + risk
		    < src->bfg[h+1].total + src->bfg[h+1].risk) {
			SUPERVERBOSE("...%s can reach here in hoplen %zu total %"PRIu64,
				     type_to_string(trc, struct pubkey,
						    &src->id),
				     h, node->bfg[h].total + fee);
			src->bfg[h+1].total = node->bfg[h].total + fee;
			src->bfg[h+1].risk = risk;
			src->bfg[h+1].prev = chan;
		}
	}
}

/* Determine if the given half_chan is routable */
static bool hc_is_routable(const struct half_chan *hc, time_t now)
{
	return hc->active && hc->unroutable_until < now;
}

/* riskfactor is already scaled to per-block amount */
static struct chan **
find_route(const tal_t *ctx, struct routing_state *rstate,
	   const struct pubkey *from, const struct pubkey *to, u64 msatoshi,
	   double riskfactor,
	   double fuzz, const struct siphash_seed *base_seed,
	   u64 *fee)
{
	struct chan **route;
	struct node *n, *src, *dst;
	struct node_map_iter it;
	int runs, i, best;
	/* Call time_now() once at the start, so that our tight loop
	 * does not keep calling into operating system for the
	 * current time */
	time_t now = time_now().ts.tv_sec;

	/* Note: we map backwards, since we know the amount of satoshi we want
	 * at the end, and need to derive how much we need to send. */
	dst = get_node(rstate, from);
	src = get_node(rstate, to);

	if (!src) {
		status_info("find_route: cannot find %s",
			    type_to_string(trc, struct pubkey, to));
		return NULL;
	} else if (!dst) {
		status_info("find_route: cannot find myself (%s)",
			    type_to_string(trc, struct pubkey, to));
		return NULL;
	} else if (dst == src) {
		status_info("find_route: this is %s, refusing to create empty route",
			    type_to_string(trc, struct pubkey, to));
		return NULL;
	}

	if (msatoshi >= MAX_MSATOSHI) {
		status_info("find_route: can't route huge amount %"PRIu64,
			    msatoshi);
		return NULL;
	}

	/* Reset all the information. */
	clear_bfg(rstate->nodes);

	/* Bellman-Ford-Gibson: like Bellman-Ford, but keep values for
	 * every path length. */
	src->bfg[0].total = msatoshi;
	src->bfg[0].risk = 0;

	for (runs = 0; runs < ROUTING_MAX_HOPS; runs++) {
		SUPERVERBOSE("Run %i", runs);
		/* Run through every edge. */
		for (n = node_map_first(rstate->nodes, &it);
		     n;
		     n = node_map_next(rstate->nodes, &it)) {
			size_t num_edges = tal_count(n->chans);
			for (i = 0; i < num_edges; i++) {
				struct chan *chan = n->chans[i];
				int idx = half_chan_to(n, chan);

				SUPERVERBOSE("Node %s edge %i/%zu",
					     type_to_string(trc, struct pubkey,
							    &n->id),
					     i, num_edges);

				if (!hc_is_routable(&chan->half[idx], now)) {
					SUPERVERBOSE("...unroutable");
					continue;
				}
				bfg_one_edge(n, chan, idx,
					     riskfactor, fuzz, base_seed);
				SUPERVERBOSE("...done");
			}
		}
	}

	best = 0;
	for (i = 1; i <= ROUTING_MAX_HOPS; i++) {
		if (dst->bfg[i].total < dst->bfg[best].total)
			best = i;
	}

	/* No route? */
	if (dst->bfg[best].total >= INFINITE) {
		status_trace("find_route: No route to %s",
			     type_to_string(trc, struct pubkey, to));
		return NULL;
	}

	/* We (dst) don't charge ourselves fees, so skip first hop */
	n = other_node(dst, dst->bfg[best].prev);
	*fee = n->bfg[best-1].total - msatoshi;

	/* Lay out route */
	route = tal_arr(ctx, struct chan *, best);
	for (i = 0, n = dst;
	     i < best;
	     n = other_node(n, n->bfg[best-i].prev), i++) {
		route[i] = n->bfg[best-i].prev;
	}
	assert(n == src);

	return route;
}

/* Verify the signature of a channel_update message */
static bool check_channel_update(const struct pubkey *node_key,
				 const secp256k1_ecdsa_signature *node_sig,
				 const u8 *update)
{
	/* 2 byte msg type + 64 byte signatures */
	int offset = 66;
	struct sha256_double hash;
	sha256_double(&hash, update + offset, tal_len(update) - offset);

	return check_signed_hash(&hash, node_sig, node_key);
}

static bool check_channel_announcement(
    const struct pubkey *node1_key, const struct pubkey *node2_key,
    const struct pubkey *bitcoin1_key, const struct pubkey *bitcoin2_key,
    const secp256k1_ecdsa_signature *node1_sig,
    const secp256k1_ecdsa_signature *node2_sig,
    const secp256k1_ecdsa_signature *bitcoin1_sig,
    const secp256k1_ecdsa_signature *bitcoin2_sig, const u8 *announcement)
{
	/* 2 byte msg type + 256 byte signatures */
	int offset = 258;
	struct sha256_double hash;
	sha256_double(&hash, announcement + offset,
		      tal_len(announcement) - offset);

	return check_signed_hash(&hash, node1_sig, node1_key) &&
	       check_signed_hash(&hash, node2_sig, node2_key) &&
	       check_signed_hash(&hash, bitcoin1_sig, bitcoin1_key) &&
	       check_signed_hash(&hash, bitcoin2_sig, bitcoin2_key);
}

static void add_pending_node_announcement(struct routing_state *rstate, struct pubkey *nodeid)
{
	struct pending_node_announce *pna = tal(rstate, struct pending_node_announce);
	pna->nodeid = *nodeid;
	pna->node_announcement = NULL;
	pna->timestamp = 0;
	pending_node_map_add(rstate->pending_node_map, pna);
}

static void process_pending_node_announcement(struct routing_state *rstate,
					      struct pubkey *nodeid)
{
	struct pending_node_announce *pna = pending_node_map_get(rstate->pending_node_map, &nodeid->pubkey);
	if (!pna)
		return;

	if (pna->node_announcement) {
		SUPERVERBOSE(
		    "Processing deferred node_announcement for node %s",
		    type_to_string(pna, struct pubkey, nodeid));
		handle_node_announcement(rstate, pna->node_announcement);
	}
	pending_node_map_del(rstate->pending_node_map, pna);
	tal_free(pna);
}

static struct pending_cannouncement *
find_pending_cannouncement(struct routing_state *rstate,
			   const struct short_channel_id *scid)
{
	struct pending_cannouncement *i;

	list_for_each(&rstate->pending_cannouncement, i, list) {
		if (structeq(scid, &i->short_channel_id))
			return i;
	}
	return NULL;
}

static void destroy_pending_cannouncement(struct pending_cannouncement *pending,
					  struct routing_state *rstate)
{
	list_del_from(&rstate->pending_cannouncement, &pending->list);
}

const struct short_channel_id *handle_channel_announcement(
	struct routing_state *rstate,
	const u8 *announce TAKES)
{
	struct pending_cannouncement *pending;
	struct bitcoin_blkid chain_hash;
	u8 *features;
	secp256k1_ecdsa_signature node_signature_1, node_signature_2;
	secp256k1_ecdsa_signature bitcoin_signature_1, bitcoin_signature_2;
	struct chan *chan;

	pending = tal(rstate, struct pending_cannouncement);
	pending->updates[0] = NULL;
	pending->updates[1] = NULL;
	pending->announce = tal_dup_arr(pending, u8,
					announce, tal_len(announce), 0);
	pending->update_timestamps[0] = pending->update_timestamps[1] = 0;

	if (!fromwire_channel_announcement(pending, pending->announce,
					   &node_signature_1,
					   &node_signature_2,
					   &bitcoin_signature_1,
					   &bitcoin_signature_2,
					   &features,
					   &chain_hash,
					   &pending->short_channel_id,
					   &pending->node_id_1,
					   &pending->node_id_2,
					   &pending->bitcoin_key_1,
					   &pending->bitcoin_key_2)) {
		tal_free(pending);
		return NULL;
	}

	/* Check if we know the channel already (no matter in what
	 * state, we stop here if yes). */
	chan = get_channel(rstate, &pending->short_channel_id);
	if (chan != NULL && chan->public) {
		SUPERVERBOSE("%s: %s already has public channel",
			     __func__,
			     type_to_string(trc, struct short_channel_id,
					    &pending->short_channel_id));
		return tal_free(pending);
	}

	/* We don't replace previous ones, since we might validate that and
	 * think this one is OK! */
	if (find_pending_cannouncement(rstate, &pending->short_channel_id)) {
		SUPERVERBOSE("%s: %s already has pending cannouncement",
			     __func__,
			     type_to_string(trc, struct short_channel_id,
					    &pending->short_channel_id));
		return tal_free(pending);
	}

	/* FIXME: Handle duplicates as per BOLT #7 */

	/* BOLT #7:
	 *
	 * If there is an unknown even bit in the `features` field the
	 * receiving node MUST NOT parse the remainder of the message
	 * and MUST NOT add the channel to its local network view, and
	 * SHOULD NOT forward the announcement.
	 */
	if (unsupported_features(features, NULL)) {
		status_trace("Ignoring channel announcement, unsupported features %s.",
			     tal_hex(pending, features));
		tal_free(pending);
		return NULL;
	}

	/* BOLT #7:
	 *
	 * The receiving node MUST ignore the message if the specified
	 * `chain_hash` is unknown to the receiver.
	 */
	if (!structeq(&chain_hash, &rstate->chain_hash)) {
		status_trace(
		    "Received channel_announcement %s for unknown chain %s",
		    type_to_string(pending, struct short_channel_id,
				   &pending->short_channel_id),
		    type_to_string(pending, struct bitcoin_blkid, &chain_hash));
		tal_free(pending);
		return NULL;
	}

	if (!check_channel_announcement(&pending->node_id_1, &pending->node_id_2,
					&pending->bitcoin_key_1,
					&pending->bitcoin_key_2,
					&node_signature_1,
					&node_signature_2,
					&bitcoin_signature_1,
					&bitcoin_signature_2,
					pending->announce)) {
		status_trace("Signature verification of channel_announcement"
			     " for %s failed",
			     type_to_string(pending, struct short_channel_id,
					    &pending->short_channel_id));
		tal_free(pending);
		return NULL;
	}

	status_trace("Received channel_announcement for channel %s",
		     type_to_string(pending, struct short_channel_id,
				    &pending->short_channel_id));

	/* Add both endpoints to the pending_node_map so we can stash
	 * node_announcements while we wait for the txout check */
	add_pending_node_announcement(rstate, &pending->node_id_1);
	add_pending_node_announcement(rstate, &pending->node_id_2);

	list_add_tail(&rstate->pending_cannouncement, &pending->list);
	tal_add_destructor2(pending, destroy_pending_cannouncement, rstate);

	return &pending->short_channel_id;
}

bool handle_pending_cannouncement(struct routing_state *rstate,
				  const struct short_channel_id *scid,
				  const u64 satoshis,
				  const u8 *outscript)
{
	bool local;
	u8 *tag;
	const u8 *s;
	struct pending_cannouncement *pending;
	struct chan *chan;

	pending = find_pending_cannouncement(rstate, scid);
	if (!pending)
		return false;

	tag = tal_arr(pending, u8, 0);
	towire_short_channel_id(&tag, scid);

	/* BOLT #7:
	 *
	 * The receiving node MUST ignore the message if this output is spent.
	 */
	if (tal_len(outscript) == 0) {
		status_trace("channel_announcement: no unspent txout %s",
			     type_to_string(pending, struct short_channel_id,
					    scid));
		tal_free(pending);
		return false;
	}

	/* BOLT #7:
	 *
	 * The receiving node MUST ignore the message if the output
	 * specified by `short_channel_id` does not correspond to a
	 * P2WSH using `bitcoin_key_1` and `bitcoin_key_2` as
	 * specified in [BOLT
	 * #3](03-transactions.md#funding-transaction-output).
	 */
	s = scriptpubkey_p2wsh(pending,
			       bitcoin_redeem_2of2(pending,
						   &pending->bitcoin_key_1,
						   &pending->bitcoin_key_2));

	if (!scripteq(s, outscript)) {
		status_trace("channel_announcement: txout %s expectes %s, got %s",
			     type_to_string(pending, struct short_channel_id,
					    scid),
			     tal_hex(trc, s), tal_hex(trc, outscript));
		tal_free(pending);
		return false;
	}

	/* The channel may already exist if it was non-public from
	 * local_add_channel(); normally we don't accept new
	 * channel_announcements.  See handle_channel_announcement. */
	chan = get_channel(rstate, scid);
	if (!chan)
		chan = new_chan(rstate, scid,
					   &pending->node_id_1,
					   &pending->node_id_2);

	/* Channel is now public. */
	chan->public = true;
	chan->satoshis = satoshis;

	/* Save channel_announcement. */
	tal_free(chan->channel_announcement);
	chan->channel_announcement = tal_steal(chan, pending->announce);

	if (replace_broadcast(rstate->broadcasts,
			      &chan->channel_announce_msgidx,
			      WIRE_CHANNEL_ANNOUNCEMENT,
			      tag, pending->announce))
		status_failed(STATUS_FAIL_INTERNAL_ERROR,
			      "Announcement %s was replaced?",
			      tal_hex(trc, pending->announce));

	local = pubkey_eq(&pending->node_id_1, &rstate->local_id) ||
		pubkey_eq(&pending->node_id_2, &rstate->local_id);

	/* Did we have an update waiting?  If so, apply now. */
	if (pending->updates[0])
		handle_channel_update(rstate, pending->updates[0]);
	if (pending->updates[1])
		handle_channel_update(rstate, pending->updates[1]);

	process_pending_node_announcement(rstate, &pending->node_id_1);
	process_pending_node_announcement(rstate, &pending->node_id_2);

	tal_free(pending);
	return local;
}

static void update_pending(struct pending_cannouncement *pending,
			   u32 timestamp, const u8 *update,
			   const u8 direction)
{
	SUPERVERBOSE("Deferring update for pending channel %s(%d)",
		     type_to_string(trc, struct short_channel_id,
				    &pending->short_channel_id), direction);

	if (pending->update_timestamps[direction] < timestamp) {
		if (pending->updates[direction]) {
			status_trace("Replacing existing update");
			tal_free(pending->updates[direction]);
		}
		pending->updates[direction] = tal_dup_arr(pending, u8, update, tal_len(update), 0);
		pending->update_timestamps[direction] = timestamp;
	}
}

void set_connection_values(struct chan *chan,
			   int idx,
			   u32 base_fee,
			   u32 proportional_fee,
			   u32 delay,
			   bool active,
			   u64 timestamp,
			   u32 htlc_minimum_msat)
{
	struct half_chan *c = &chan->half[idx];

	c->delay = delay;
	c->htlc_minimum_msat = htlc_minimum_msat;
	c->base_fee = base_fee;
	c->proportional_fee = proportional_fee;
	c->active = active;
	c->last_timestamp = timestamp;
	assert((c->flags & 0x1) == idx);

	/* If it was temporarily unroutable, re-enable */
	c->unroutable_until = 0;

	SUPERVERBOSE("Channel %s(%d) was updated.",
		     type_to_string(trc, struct short_channel_id, &chan->scid),
		     idx);

	if (c->proportional_fee >= MAX_PROPORTIONAL_FEE) {
		status_trace("Channel %s(%d) massive proportional fee %u:"
			     " disabling.",
			     type_to_string(trc, struct short_channel_id,
					    &chan->scid),
			     idx,
			     c->proportional_fee);
		c->active = false;
	}
}

void handle_channel_update(struct routing_state *rstate, const u8 *update)
{
	u8 *serialized;
	struct half_chan *c;
	secp256k1_ecdsa_signature signature;
	struct short_channel_id short_channel_id;
	u32 timestamp;
	u16 flags;
	u16 expiry;
	u64 htlc_minimum_msat;
	u32 fee_base_msat;
	u32 fee_proportional_millionths;
	const tal_t *tmpctx = tal_tmpctx(rstate);
	struct bitcoin_blkid chain_hash;
	struct chan *chan;
	u8 direction;
	size_t len = tal_len(update);

	serialized = tal_dup_arr(tmpctx, u8, update, len, 0);
	if (!fromwire_channel_update(serialized, &signature,
				     &chain_hash, &short_channel_id,
				     &timestamp, &flags, &expiry,
				     &htlc_minimum_msat, &fee_base_msat,
				     &fee_proportional_millionths)) {
		tal_free(tmpctx);
		return;
	}
	direction = flags & 0x1;

	/* BOLT #7:
	 *
	 * The receiving node MUST ignore the channel update if the specified
	 * `chain_hash` value is unknown, meaning it isn't active on the
	 * specified chain. */
	if (!structeq(&chain_hash, &rstate->chain_hash)) {
		status_trace("Received channel_update for unknown chain %s",
			     type_to_string(tmpctx, struct bitcoin_blkid,
					    &chain_hash));
		tal_free(tmpctx);
		return;
	}

	chan = get_channel(rstate, &short_channel_id);

	/* Optimization: only check for pending if not public */
	if (!chan || !chan->public) {
		struct pending_cannouncement *pending;

		pending = find_pending_cannouncement(rstate, &short_channel_id);
		if (pending) {
			update_pending(pending,
				       timestamp, serialized, direction);
			tal_free(tmpctx);
			return;
		}

		if (!chan) {
			SUPERVERBOSE("Ignoring update for unknown channel %s",
				     type_to_string(trc, struct short_channel_id,
						    &short_channel_id));
			tal_free(tmpctx);
			return;
		}
	}

	c = &chan->half[direction];

	if (c->last_timestamp >= timestamp) {
		SUPERVERBOSE("Ignoring outdated update.");
		tal_free(tmpctx);
		return;
	}

	if (!check_channel_update(&chan->nodes[direction]->id,
				  &signature, serialized)) {
		status_trace("Signature verification failed.");
		tal_free(tmpctx);
		return;
	}

	status_trace("Received channel_update for channel %s(%d) now %s",
		     type_to_string(trc, struct short_channel_id,
				    &short_channel_id),
		     flags & 0x01,
		     flags & ROUTING_FLAGS_DISABLED ? "DISABLED" : "ACTIVE");

	set_connection_values(chan, direction,
			      fee_base_msat,
			      fee_proportional_millionths,
			      expiry,
			      (flags & ROUTING_FLAGS_DISABLED) == 0,
			      timestamp,
			      htlc_minimum_msat);

	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_short_channel_id(&tag, &short_channel_id);
	towire_u16(&tag, direction);
	replace_broadcast(rstate->broadcasts,
			  &chan->half[direction].channel_update_msgidx,
			WIRE_CHANNEL_UPDATE,
			tag,
			serialized);

	tal_free(c->channel_update);
	c->channel_update = tal_steal(chan, serialized);
	tal_free(tmpctx);
}

static struct wireaddr *read_addresses(const tal_t *ctx, const u8 *ser)
{
	const u8 *cursor = ser;
	size_t max = tal_len(ser);
	struct wireaddr *wireaddrs = tal_arr(ctx, struct wireaddr, 0);
	int numaddrs = 0;
	while (cursor && cursor < ser + max) {
		struct wireaddr wireaddr;

		/* Skip any padding */
		while (max && cursor[0] == ADDR_TYPE_PADDING)
			fromwire_u8(&cursor, &max);

		/* BOLT #7:
		 *
		 * The receiving node SHOULD ignore the first `address
		 * descriptor` which does not match the types defined
		 * above.
		 */
		if (!fromwire_wireaddr(&cursor, &max, &wireaddr)) {
			if (!cursor)
				/* Parsing address failed */
				return tal_free(wireaddrs);
			/* Unknown type, stop there. */
			break;
		}

		tal_resize(&wireaddrs, numaddrs+1);
		wireaddrs[numaddrs] = wireaddr;
		numaddrs++;
	}
	return wireaddrs;
}

void handle_node_announcement(
	struct routing_state *rstate, const u8 *node_ann)
{
	u8 *serialized;
	struct sha256_double hash;
	struct node *node;
	secp256k1_ecdsa_signature signature;
	u32 timestamp;
	struct pubkey node_id;
	u8 rgb_color[3];
	u8 alias[32];
	u8 *features, *addresses;
	const tal_t *tmpctx = tal_tmpctx(rstate);
	struct wireaddr *wireaddrs;
	struct pending_node_announce *pna;
	size_t len = tal_len(node_ann);

	serialized = tal_dup_arr(tmpctx, u8, node_ann, len, 0);
	if (!fromwire_node_announcement(tmpctx, serialized,
					&signature, &features, &timestamp,
					&node_id, rgb_color, alias,
					&addresses)) {
		tal_free(tmpctx);
		return;
	}

	/* BOLT #7:
	 *
	 * If the `features` field contains unknown even bits the
	 * receiving node MUST NOT parse the remainder of the message
	 * and MAY discard the message altogether.
	 */
	if (unsupported_features(features, NULL)) {
		status_trace("Ignoring node announcement for node %s, unsupported features %s.",
			     type_to_string(tmpctx, struct pubkey, &node_id),
			     tal_hex(tmpctx, features));
		tal_free(tmpctx);
		return;
	}

	sha256_double(&hash, serialized + 66, tal_count(serialized) - 66);
	if (!check_signed_hash(&hash, &signature, &node_id)) {
		status_trace("Ignoring node announcement, signature verification failed.");
		tal_free(tmpctx);
		return;
	}

	node = get_node(rstate, &node_id);

	/* Check if we are currently verifying the txout for a
	 * matching channel */
	pna = pending_node_map_get(rstate->pending_node_map, &node_id.pubkey);
	if (!node && pna) {
		if (pna->timestamp < timestamp) {
			SUPERVERBOSE(
			    "Deferring node_announcement for node %s",
			    type_to_string(tmpctx, struct pubkey, &node_id));
			pna->timestamp = timestamp;
			tal_free(pna->node_announcement);
			pna->node_announcement = tal_dup_arr(pna, u8, node_ann, tal_len(node_ann), 0);
		}
		tal_free(tmpctx);
		return;
	}

	if (!node) {
		SUPERVERBOSE("Node not found, was the node_announcement for "
			     "node %s preceded by at least "
			     "channel_announcement?",
			     type_to_string(tmpctx, struct pubkey, &node_id));
		tal_free(tmpctx);
		return;
	} else if (node->last_timestamp >= timestamp) {
		SUPERVERBOSE("Ignoring node announcement, it's outdated.");
		tal_free(tmpctx);
		return;
	}

	status_trace("Received node_announcement for node %s",
		     type_to_string(tmpctx, struct pubkey, &node_id));

	wireaddrs = read_addresses(tmpctx, addresses);
	if (!wireaddrs) {
		status_trace("Unable to parse addresses.");
		tal_free(serialized);
		return;
	}
	tal_free(node->addresses);
	node->addresses = tal_steal(node, wireaddrs);

	node->last_timestamp = timestamp;

	memcpy(node->rgb_color, rgb_color, 3);
	tal_free(node->alias);
	node->alias = tal_dup_arr(node, u8, alias, 32, 0);

	u8 *tag = tal_arr(tmpctx, u8, 0);
	towire_pubkey(&tag, &node_id);
	replace_broadcast(rstate->broadcasts,
			  &node->announcement_idx,
			  WIRE_NODE_ANNOUNCEMENT,
			  tag,
			  serialized);
	tal_free(node->node_announcement);
	node->node_announcement = tal_steal(node, serialized);
	tal_free(tmpctx);
}

struct route_hop *get_route(tal_t *ctx, struct routing_state *rstate,
			    const struct pubkey *source,
			    const struct pubkey *destination,
			    const u32 msatoshi, double riskfactor,
			    u32 final_cltv,
			    double fuzz, const struct siphash_seed *base_seed)
{
	struct chan **route;
	u64 total_amount;
	unsigned int total_delay;
	u64 fee;
	struct route_hop *hops;
	int i;
	struct node *n;

	route = find_route(ctx, rstate, source, destination, msatoshi,
			   riskfactor / BLOCKS_PER_YEAR / 10000,
			   fuzz, base_seed, &fee);

	if (!route) {
		return NULL;
	}

	/* Fees, delays need to be calculated backwards along route. */
	hops = tal_arr(ctx, struct route_hop, tal_count(route));
	total_amount = msatoshi;
	total_delay = final_cltv;

	/* Start at destination node. */
	n = get_node(rstate, destination);
	for (i = tal_count(route) - 1; i >= 0; i--) {
		const struct half_chan *c;
		int idx = half_chan_to(n, route[i]);
		c = &route[i]->half[idx];
		hops[i].channel_id = route[i]->scid;
		hops[i].nodeid = n->id;
		hops[i].amount = total_amount;
		hops[i].delay = total_delay;
		total_amount += connection_fee(c, total_amount);
		total_delay += c->delay;
		n = other_node(n, route[i]);
	}
	assert(pubkey_eq(&n->id, source));

	/* FIXME: Shadow route! */
	return hops;
}

/**
 * routing_failure_channel_out - Handle routing failure on a specific channel
 *
 * If we want to delete the channel, we reparent it to disposal_context.
 */
static void routing_failure_channel_out(const tal_t *disposal_context,
					struct node *node,
					enum onion_type failcode,
					struct chan *chan,
					time_t now)
{
	struct half_chan *hc = half_chan_from(node, chan);

	/* BOLT #4:
	 *
	 * - if the PERM bit is NOT set:
	 *   - SHOULD restore the channels as it receives new `channel_update`s.
	 */
	if (!(failcode & PERM))
		/* Prevent it for 20 seconds. */
		hc->unroutable_until = now + 20;
	else
		/* Set it up to be pruned. */
		tal_steal(disposal_context, chan);
}

void routing_failure(struct routing_state *rstate,
		     const struct pubkey *erring_node_pubkey,
		     const struct short_channel_id *scid,
		     enum onion_type failcode,
		     const u8 *channel_update)
{
	const tal_t *tmpctx = tal_tmpctx(rstate);
	struct node *node;
	int i;
	enum wire_type t;
	time_t now = time_now().ts.tv_sec;

	status_trace("Received routing failure 0x%04x (%s), "
		     "erring node %s, "
		     "channel %s",
		     (int) failcode, onion_type_name(failcode),
		     type_to_string(tmpctx, struct pubkey, erring_node_pubkey),
		     type_to_string(tmpctx, struct short_channel_id, scid));

	node = get_node(rstate, erring_node_pubkey);
	if (!node) {
		status_unusual("routing_failure: Erring node %s not in map",
			       type_to_string(tmpctx, struct pubkey,
					      erring_node_pubkey));
		/* No node, so no channel, so any channel_update
		 * can also be ignored. */
		goto out;
	}

	/* BOLT #4:
	 *
	 * - if the NODE bit is set:
	 *   - SHOULD remove all channels connected with the erring node from
	 *   consideration.
	 *
	 */
	if (failcode & NODE) {
		for (i = 0; i < tal_count(node->chans); ++i) {
			routing_failure_channel_out(tmpctx, node, failcode,
						    node->chans[i],
						    now);
		}
	} else {
		struct chan *chan = get_channel(rstate, scid);

		if (!chan)
			status_unusual("routing_failure: "
				       "Channel %s unknown",
				       type_to_string(tmpctx,
						      struct short_channel_id,
						      scid));
		else if (chan->nodes[0] != node && chan->nodes[1] != node)
			status_unusual("routing_failure: "
				       "Channel %s does not connect to %s",
				       type_to_string(tmpctx,
						      struct short_channel_id,
						      scid),
				       type_to_string(tmpctx, struct pubkey,
						      erring_node_pubkey));
		else
			routing_failure_channel_out(tmpctx,
						    node, failcode, chan, now);
	}

	/* Update the channel if UPDATE failcode. Do
	 * this after deactivating, so that if the
	 * channel_update is newer it will be
	 * reactivated. */
	if (failcode & UPDATE) {
		if (tal_len(channel_update) == 0) {
			/* Suppress UNUSUAL log if local failure */
			if (structeq(&erring_node_pubkey->pubkey,
				     &rstate->local_id.pubkey))
				goto out;
			status_unusual("routing_failure: "
				       "UPDATE bit set, no channel_update. "
				       "failcode: 0x%04x",
				       (int) failcode);
			goto out;
		}
		t = fromwire_peektype(channel_update);
		if (t != WIRE_CHANNEL_UPDATE) {
			status_unusual("routing_failure: "
				       "not a channel_update. "
				       "type: %d",
				       (int) t);
			goto out;
		}
		handle_channel_update(rstate, channel_update);
	} else {
		if (tal_len(channel_update) != 0)
			status_unusual("routing_failure: "
				       "UPDATE bit clear, channel_update given. "
				       "failcode: 0x%04x",
				       (int) failcode);
	}

out:
	tal_free(tmpctx);
}

void mark_channel_unroutable(struct routing_state *rstate,
			     const struct short_channel_id *channel)
{
	const tal_t *tmpctx = tal_tmpctx(rstate);
	struct chan *chan;
	time_t now = time_now().ts.tv_sec;
	const char *scid = type_to_string(tmpctx, struct short_channel_id,
					  channel);

	status_trace("Received mark_channel_unroutable channel %s",
		     scid);

	chan = get_channel(rstate, channel);
	if (!chan) {
		status_unusual("mark_channel_unroutable: "
			       "channel %s not in routemap",
			       scid);
		tal_free(tmpctx);
		return;
	}
	chan->half[0].unroutable_until = now + 20;
	chan->half[1].unroutable_until = now + 20;
	tal_free(tmpctx);
}

void route_prune(struct routing_state *rstate)
{
	u64 now = time_now().ts.tv_sec;
	/* Anything below this highwater mark ought to be pruned */
	const s64 highwater = now - rstate->prune_timeout;
	const tal_t *pruned = tal_tmpctx(rstate);
	struct chan *chan;
	u64 idx;

	/* Now iterate through all channels and see if it is still alive */
	for (chan = uintmap_first(&rstate->chanmap, &idx);
	     chan;
	     chan = uintmap_after(&rstate->chanmap, &idx)) {
		/* Local-only?  Don't prune. */
		if (!chan->public)
			continue;

		if (chan->half[0].last_timestamp < highwater
		    && chan->half[1].last_timestamp < highwater) {
			status_trace(
			    "Pruning channel %s from network view (ages %"PRIu64" and %"PRIu64"s)",
			    type_to_string(trc, struct short_channel_id,
					   &chan->scid),
			    now - chan->half[0].last_timestamp,
			    now - chan->half[1].last_timestamp);

			/* This may perturb iteration so do outside loop. */
			tal_steal(pruned, chan);
		}
	}

	/* This frees all the chans and maybe even nodes. */
	tal_free(pruned);
}
