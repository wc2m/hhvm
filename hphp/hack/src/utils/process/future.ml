(**
 * Copyright (c) 2016, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 *)

(**
 * We shell out hg commands, so its computation is out-of-process, and
 * return a Future for the result.
 *
 * A promise is a strongly-typed wrapper around Process
 *)

include Future_sig.Types

type 'a promise =
  | Complete : 'a -> 'a promise
  | Complete_but_transformer_raised of (Process_types.info * exn)
    (** Delayed is useful for testing. Must be tapped by "is ready"
     * the given number of times before it is ready. *)
  | Delayed : (int * 'a) -> 'a promise
  | Incomplete of Process_types.t * (string -> 'a)
type 'a t = 'a promise ref

let make process transformer =
  ref (Incomplete (process, transformer))

let of_value v = ref @@ Complete v

let delayed_value ~delays v =
  ref (Delayed (delays, v))

let error_to_string (info, e) =
  let info = Printf.sprintf "(%s [%s])"
    info.Process_types.name
    (String.concat ", " info.Process_types.args) in
  let status_string s = match s with
    | Unix.WEXITED i -> Printf.sprintf "(%s WEXITED %d)" info i
    | Unix.WSIGNALED i -> Printf.sprintf "(%s WSIGNALED %d)" info i
    | Unix.WSTOPPED i -> Printf.sprintf "(%s WSTOPPED %d)" info i
  in
  match e with
  | Process_failure (status, stderr) ->
    Printf.sprintf "Process_failure(%s, stderr: %s)" (status_string status) stderr
  | Timed_out (stdout , stderr) ->
    Printf.sprintf "Timed_out((stdout: %s) (stderr: %s))" stdout stderr
  | Process_aborted ->
    "Process_aborted"
  | Transformer_raised e ->
    Printf.sprintf "Transformer_raised(%s)" (Printexc.to_string e)

let error_to_exn e = raise (Failure e)

let get : 'a t -> ('a, error) result =
  fun promise -> match !promise with
  | Complete v -> Ok v
  | Complete_but_transformer_raised (info, e) ->
    Error (info, Transformer_raised e)
  | Delayed (i, v) when i <= 0 ->
    Ok v
  | Delayed _ ->
    Error (Process_types.dummy.Process_types.info, Timed_out ("", "Delayed value not ready yet"))
  | Incomplete (process, transformer) ->
    let info = process.Process_types.info in
    match Process.read_and_wait_pid ~timeout:30 process with
    | Ok (stdout, _stderr) -> begin
      try
        let result = transformer stdout in
        let () = promise := Complete result in
        Ok result
      with
      | e ->
        let () = promise := (Complete_but_transformer_raised (info, e)) in
        Error (info, Transformer_raised e)
    end
    | Error (Process_types.Process_exited_abnormally
        (status, _, stderr)) ->
      Error (info, Process_failure (status, stderr))
    | Error (Process_types.Timed_out (stdout, stderr)) ->
      Error (info, Timed_out (stdout, stderr))
    | Error Process_types.Process_aborted_input_too_large ->
      Error (info, Process_aborted)

let get_exn x = get x
  |> Core_result.map_error ~f:error_to_exn
  |> Core_result.ok_exn

let is_ready promise = match !promise with
  | Complete _ | Complete_but_transformer_raised _ -> true
  | Delayed (i, _) when i <= 0 -> true
  | Delayed (i, v) ->
    promise := Delayed(i - 1, v);
    false
  | Incomplete (process, _) ->
    Process.is_ready process
