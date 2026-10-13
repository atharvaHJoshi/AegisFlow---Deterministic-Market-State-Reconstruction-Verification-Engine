(* Independent reference model for MarketState, written in OCaml.

   This deliberately mirrors reference/python/model.py in *behaviour and output*
   while being a completely separate implementation: price levels are recomputed
   from the raw order set at serialization time (rather than maintained
   incrementally as the C++ engine does). Producing byte-identical canonical
   state is the project's correctness oracle.

   It shares the same CLI as the Python model so the verification harness can
   use either implementation as the reference:
     model --events FILE [--state-out FILE] [--summary-out FILE]
   and prints a JSON summary to stdout. *)

let fnv_offset = 1469598103934665603L
let fnv_prime = 1099511628211L

let fnv1a (s : string) : int64 =
  let h = ref fnv_offset in
  String.iter
    (fun c ->
      h := Int64.logxor !h (Int64.of_int (Char.code c));
      h := Int64.mul !h fnv_prime)
    s;
  !h

let order_add = 0
let order_cancel = 1
let order_modify = 2
let trade = 3
let market_update = 4
let bid = 0
let ask = 1
let no_price = -1
let no_quantity = -1

let payload_tokens = function
  | 0 -> Some 4
  | 1 -> Some 2
  | 2 -> Some 3
  | 3 -> Some 4
  | 4 -> Some 2
  | _ -> None

type event = {
  event_id : int;
  stream_id : int;
  seq : int;
  symbol : string;
  etype : int;
  timestamp : int;
  payload : int list;
  checksum : int64;
  valid : bool;
  error : string;
}

let is_space c = c = ' ' || c = '\t' || c = '\r' || c = '\n' || c = '\011' || c = '\012'

let split_ws (s : string) : string list =
  let n = String.length s in
  let out = ref [] in
  let buf = Buffer.create 16 in
  let flush () =
    if Buffer.length buf > 0 then begin
      out := Buffer.contents buf :: !out;
      Buffer.clear buf
    end
  in
  for i = 0 to n - 1 do
    if is_space s.[i] then flush () else Buffer.add_char buf s.[i]
  done;
  flush ();
  List.rev !out

(* Decimal-only integer parsing, matching Python's int() semantics: no hex,
   octal or binary prefixes, optional leading sign. *)
let parse_int_dec (s : string) : int option =
  let n = String.length s in
  if n = 0 then None
  else begin
    let start = if s.[0] = '+' || s.[0] = '-' then 1 else 0 in
    if start >= n then None
    else begin
      let ok = ref true in
      for i = start to n - 1 do
        if not (s.[i] >= '0' && s.[i] <= '9') then ok := false
      done;
      if not !ok then None else int_of_string_opt s
    end
  end

(* 64-bit unsigned decimal parse; wraps modulo 2^64 exactly like a u64. *)
let parse_int64_dec (s : string) : int64 option =
  let n = String.length s in
  if n = 0 then None
  else begin
    let neg = s.[0] = '-' in
    let start = if neg || s.[0] = '+' then 1 else 0 in
    if start >= n then None
    else begin
      let ok = ref true in
      for i = start to n - 1 do
        if not (s.[i] >= '0' && s.[i] <= '9') then ok := false
      done;
      if not !ok then None
      else begin
        let h = ref 0L in
        for i = start to n - 1 do
          h :=
            Int64.add
              (Int64.mul !h 10L)
              (Int64.of_int (Char.code s.[i] - Char.code '0'))
        done;
        Some (if neg then Int64.neg !h else !h)
      end
    end
  end

let parse_event (line : string) : event option =
  let toks = Array.of_list (split_ws line) in
  let n = Array.length toks in
  if n < 8 then None
  else
    match
      ( parse_int_dec toks.(0),
        parse_int_dec toks.(1),
        parse_int_dec toks.(2),
        parse_int_dec toks.(4),
        parse_int_dec toks.(5),
        parse_int64_dec toks.(n - 1) )
    with
    | ( Some event_id,
        Some stream_id,
        Some seq,
        Some etype,
        Some timestamp,
        Some checksum ) -> (
        let symbol = toks.(3) in
        let mk valid error payload =
          {
            event_id;
            stream_id;
            seq;
            symbol;
            etype;
            timestamp;
            payload;
            checksum;
            valid;
            error;
          }
        in
        match payload_tokens etype with
        | None -> Some (mk false "wrong payload arity" [])
        | Some need ->
            if n <> 7 + need then Some (mk false "wrong payload arity" [])
            else begin
              let rec collect i acc =
                if i > n - 2 then Some (List.rev acc)
                else
                  match parse_int_dec toks.(i) with
                  | Some v -> collect (i + 1) (v :: acc)
                  | None -> None
              in
              match collect 6 [] with
              | None -> Some (mk false "bad payload" [])
              | Some payload ->
                  let covered =
                    String.concat " "
                      (Array.to_list (Array.sub toks 0 (n - 1)))
                  in
                  if fnv1a covered <> checksum then
                    Some (mk false "checksum mismatch" payload)
                  else if symbol = "" then
                    Some (mk false "empty symbol" payload)
                  else if seq <= 0 then
                    Some (mk false "non-positive sequence" payload)
                  else Some (mk true "" payload)
            end)
    | _ -> None

type order = { oid : int; mutable side : int; mutable price : int; mutable qty : int }

type book = {
  symbol : string;
  orders : (int, order) Hashtbl.t;
  mutable total_volume : int;
  mutable total_trades : int;
  mutable last_seq : int;
}

let create_book symbol =
  {
    symbol;
    orders = Hashtbl.create 1024;
    total_volume = 0;
    total_trades = 0;
    last_seq = 0;
  }

let book_apply (b : book) (ev : event) : unit =
  if ev.seq > b.last_seq then b.last_seq <- ev.seq;
  match ev.etype with
  | 0 -> (
      match ev.payload with
      | [ oid; side; price; qty ] ->
          if (not (Hashtbl.mem b.orders oid)) && price >= 0 && qty > 0 then
            Hashtbl.replace b.orders oid { oid; side; price; qty }
      | _ -> ())
  | 1 -> (
      match ev.payload with
      | [ oid; qty ] -> (
          match Hashtbl.find_opt b.orders oid with
          | None -> ()
          | Some o ->
              let remaining = if qty = no_quantity then o.qty else min qty o.qty in
              o.qty <- o.qty - remaining;
              if o.qty <= 0 then Hashtbl.remove b.orders oid)
      | _ -> ())
  | 2 -> (
      match ev.payload with
      | [ oid; new_price; new_qty ] -> (
          match Hashtbl.find_opt b.orders oid with
          | None -> ()
          | Some o ->
              if new_price <> no_price then o.price <- new_price;
              if new_qty <> no_quantity then o.qty <- new_qty;
              if o.qty <= 0 then Hashtbl.remove b.orders oid)
      | _ -> ())
  | 3 -> (
      match ev.payload with
      | [ _buy; _sell; _price; qty ] ->
          b.total_volume <- b.total_volume + qty;
          b.total_trades <- b.total_trades + 1
      | _ -> ())
  | _ -> ()

let book_canonical (b : book) : string =
  let levels_bid = Hashtbl.create 64 in
  let levels_ask = Hashtbl.create 64 in
  let active = ref [] in
  Hashtbl.iter
    (fun _ o ->
      if o.qty > 0 then begin
        active := o :: !active;
        let tbl = if o.side = bid then levels_bid else levels_ask in
        let prev = match Hashtbl.find_opt tbl o.price with Some v -> v | None -> 0 in
        Hashtbl.replace tbl o.price (prev + o.qty)
      end)
    b.orders;
  let active = List.sort (fun a c -> compare a.oid c.oid) !active in
  let buf = Buffer.create 256 in
  Buffer.add_string buf (Printf.sprintf "STATE %s\n" b.symbol);
  Buffer.add_string buf (Printf.sprintf "SEQ %d\n" b.last_seq);
  Buffer.add_string buf (Printf.sprintf "VOL %d\n" b.total_volume);
  Buffer.add_string buf (Printf.sprintf "TRD %d\n" b.total_trades);
  Buffer.add_string buf
    (Printf.sprintf "LEVELS %d %d\n" (Hashtbl.length levels_bid)
       (Hashtbl.length levels_ask));
  List.iter
    (fun o ->
      Buffer.add_string buf
        (Printf.sprintf "O %d %d %d %d\n" o.oid o.side o.price o.qty))
    active;
  Buffer.add_string buf "END";
  Buffer.contents buf

let separator = "\n===BOOK===\n"

type model = { books : (string, book) Hashtbl.t }

let create_model () = { books = Hashtbl.create 8 }

let model_apply (m : model) (ev : event) : bool =
  if not ev.valid then false
  else begin
    let b =
      match Hashtbl.find_opt m.books ev.symbol with
      | Some b -> b
      | None ->
          let b = create_book ev.symbol in
          Hashtbl.replace m.books ev.symbol b;
          b
    in
    book_apply b ev;
    true
  end

let sorted_symbols (m : model) : string list =
  List.sort String.compare
    (Hashtbl.fold (fun k _ acc -> k :: acc) m.books [])

let model_canonical (m : model) : string =
  String.concat separator
    (List.map
       (fun s -> book_canonical (Hashtbl.find m.books s))
       (sorted_symbols m))

let state_hash (s : string) : string = Printf.sprintf "%016Lx" (fnv1a s)

let trim (s : string) : string =
  let n = String.length s in
  let i = ref 0 in
  while !i < n && is_space s.[!i] do incr i done;
  let j = ref (n - 1) in
  while !j >= !i && is_space s.[!j] do decr j done;
  if !j < !i then "" else String.sub s !i (!j - !i + 1)

let load_events (path : string) : event list =
  let ic = open_in path in
  let events = ref [] in
  (try
     while true do
       let line = trim (input_line ic) in
       if line <> "" && line.[0] <> '#' then
         match parse_event line with
         | Some ev -> events := ev :: !events
         | None -> ()
     done
   with End_of_file -> ());
  close_in ic;
  List.rev !events

let write_file (path : string) (content : string) : unit =
  let oc = open_out path in
  output_string oc content;
  close_out oc

let () =
  let events_path = ref None in
  let state_out = ref None in
  let summary_out = ref None in
  let rec parse = function
    | [] -> ()
    | "--events" :: v :: rest ->
        events_path := Some v;
        parse rest
    | "--state-out" :: v :: rest ->
        state_out := Some v;
        parse rest
    | "--summary-out" :: v :: rest ->
        summary_out := Some v;
        parse rest
    | _ :: rest -> parse rest
  in
  parse (List.tl (Array.to_list Sys.argv));
  match !events_path with
  | None ->
      prerr_endline "error: --events is required";
      exit 2
  | Some path ->
      let events = load_events path in
      let m = create_model () in
      let applied = ref 0 in
      List.iter (fun ev -> if model_apply m ev then incr applied) events;
      let state = model_canonical m in
      (match !state_out with Some p -> write_file p state | None -> ());
      let syms_json =
        "["
        ^ String.concat ", "
            (List.map (Printf.sprintf "%S") (sorted_symbols m))
        ^ "]"
      in
      let summary =
        Printf.sprintf
          "{\"events_read\": %d, \"events_applied\": %d, \"symbols\": %s, \
           \"state_hash\": \"%s\", \"state_bytes\": %d}"
          (List.length events) !applied syms_json (state_hash state)
          (String.length state)
      in
      (match !summary_out with Some p -> write_file p summary | None -> ());
      print_endline summary
