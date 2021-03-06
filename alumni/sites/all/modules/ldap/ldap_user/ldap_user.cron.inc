<?php

/**
 * @file
 * cron relate functions
 *
 */

/**
 * function to respond to ldap associated drupal accounts which no
 * longer have a related LDAP entry
 *
 * @param LdapUserConf $ldap_user_conf
 *
 * @return boolean FALSE on error or incompletion or TRUE otherwise
 *
 *  @todo need to avoid sending repeated emails
 *
 *
 */

function _ldap_user_orphans($ldap_user_conf) {

  // return TRUE; // this is untested code

  if (!$ldap_user_conf->orphanedDrupalAcctBehavior ||
    $ldap_user_conf->orphanedDrupalAcctBehavior == 'ldap_user_orphan_do_not_check') {
    return TRUE;
  }

  $last_orphan_checked = variable_get('ldap_user_cron_last_orphan_checked', 0);

  /**
   * query drupal accounts
   *   - ldap associated drupal accounts
   *   - where (ldap_user_current_dn not null)
   *   - ordered by ldap_user_last_checked
   *   - order by uid asc (get oldest first)
   */


  $query = new EntityFieldQuery();
  $query->entityCondition('entity_type', 'user')
    ->fieldCondition('ldap_user_current_dn', 'value', 'NULL', '!=')
    ->propertyCondition('uid', 'value', $last_orphan_checked, '>')
    ->propertyOrderBy('uid', 'ASC')
    ->range(0, $ldap_user_conf->orphanedCheckQty - 1)
    ->addMetaData('account', user_load(1)); // run the query as user 1
  $result = $query->execute();

  //debug("ldap_user_orphan: "); debug(count(array_keys($result['user'])));
  // if maxed out reset uid check counter
  if (count($result) < $ldap_user_conf->orphanedCheckQty) {
    variable_set('ldap_user_cron_last_orphan_checked', 0);
  }
  //dpm($result);  dpm(variable_get('ldap_user_cron_last_orphan_checked', 0));
// debug($result);  debug("_ldap_user_orphans, last orphan checked=" . variable_get('ldap_user_cron_last_orphan_checked', 0));
  $drupal_users = array();
  $ldap_servers = ldap_servers_get_servers(NULL, 'enabled');
  $watchdogs_sids_missing_watchdogged = array();
  /**
   * first produce array of form:
   *  $drupal_users[$sid][$puid_attr][$puid]['exists'] = FALSE | TRUE;
   *  signifying if corresponding LDAP Entry exists
   */
  if (!(isset($result['user']) && count($result['user']) > 0)) {
    return TRUE;
  }

  // make sure not to delete these 2 uids
  unset($result['user'][0]);
  unset($result['user'][1]);
 // dpm($result['user']);


  $uids = array_keys($result['user']);
  $user_count = count($uids);
  $batches = floor($user_count / LDAP_SERVERS_MAXFILTER_ORS) + 1;
  // e.g. 175 users and  50 max ldap query ors will yield 4 batches
  //debug("user_count=$user_count, batches=$batches");
  for ($batch=1; $batch <= $batches; $batch++) { // e.g. 1,2,3,4
    $filters = array();

    // 1. populate $drupal_users[$sid][$puid_attr][$puid]['exists']  = TRUE
    /**
     * e.g.  first batch $i=0; $i<50; $i++
     *       2nd batch   $i=50; $i<100; $i++
     *       4th batch   $i=150; $i<175; $i++
     */
    $start = ($batch - 1)* LDAP_SERVERS_MAXFILTER_ORS; // e.g 0, 50, 100
    $end_plus_1 = min(($batch)* LDAP_SERVERS_MAXFILTER_ORS, $user_count); // e.g. 50, 100, 150
    $batch_uids = array_slice($uids, $start, ($end_plus_1 - $start)); // e.g. 50, 50; 100, 50
    $accounts = entity_load('user', $batch_uids);
   // dpm("start, end+1: $start, $end_plus_1"); dpm($accounts);
  //  debug("start, end+1: $start, $end_plus_1"); debug($accounts);
    foreach ($accounts as $uid => $user) {

      $sid = @$user->ldap_user_puid_sid['und'][0]['value'];
      $puid = @$user->ldap_user_puid['und'][0]['value'];
      $puid_attr = @$user->ldap_user_puid_property['und'][0]['value'];
      //dpm("uid=$uid, sid=$sid, puid=$puid, puid_attr=$puid_attr");
      if ($sid && $puid && $puid_attr) {
        $filters[$sid][$puid_attr][] = "($puid_attr=$puid)";
        $drupal_users[$sid][$puid_attr][$puid]['uid'] = $uid;
        $drupal_users[$sid][$puid_attr][$puid]['exists'] = FALSE;
      }
      else {
        // user with missing ldap data fields
        // perhaps should be watchdogged?
      }
    }

    //dpm("filters and drupal_users, batch $batch"); dpm($filters); dpm($drupal_users);
    //2. set $drupal_users[$sid][$puid_attr][$puid]['exists'] to FALSE
    // if entry doesn't exist
    foreach ($filters as $sid => $puid_attrs) {

      if (!isset($ldap_servers[$sid])) {
        if (!isset($watchdogs_sids_missing_watchdogged[$sid])) {
          watchdog(
            'ldap_user',
            'Server %sid not enabled, but needed to remove orphaned ldap users',
            array('%sid' => $sid),
            WATCHDOG_ERROR
          );
          $watchdogs_sids_missing_watchdogged[$sid] = TRUE;
        }
        continue;
      }
      foreach ($puid_attrs as $puid_attr => $ors) {
        // query should look like (|(guid=3243243)(guid=3243243)(guid=3243243))
        $ldap_filter = '(|' . join("", $ors) . ')';
        $ldap_entries = $ldap_servers[$sid]->searchAllBaseDns($ldap_filter, array($puid_attr));
        if ($ldap_entries === FALSE) {
          unset($drupal_users[$sid]); // if query has error, don't remove ldap entries!
          watchdog(
            'ldap_user',
            'ldap server %sid had error while querying to
            deal with orphaned ldap user entries.  Please check that the ldap
            server is configured correctly.  Query; %query',
            array('%sid' => $sid, '%query' => $query),
            WATCHDOG_ERROR);
          continue;
        }

        unset($ldap_entries['count']);
       // dpm("ldap_entries"); dpm($ldap_entries);
        foreach ($ldap_entries as $i => $ldap_entry) {
          $puid = $ldap_entry[$puid_attr][0];
          $drupal_users[$sid][$puid_attr][$puid]['exists'] = TRUE;
        }
      }
    }
  }
  //3. we now have $drupal_users[$sid][$puid_attr][$puid]['exists'] = FALSE | TRUE;
  if ($ldap_user_conf->orphanedDrupalAcctBehavior == 'ldap_user_orphan_email') {
    $email_list = array();
    global $base_url;
  }
  $check_time = time();
  $email_list = array();
  foreach ($drupal_users as $sid => $puid_x_puid_attrs) {
    foreach ($puid_x_puid_attrs as $puid_attr => $puids) {
      // debug("orphaned puid data"); debug($puids);
      foreach ($puids as $puid => $user_data) {
        $account = $accounts[$user_data['uid']];
        $user_edit['ldap_user_last_checked'][LANGUAGE_NONE][0]['value'] = $check_time;
        $account = user_save($account, $user_edit, 'ldap_user');
        if (!$user_data['exists']) {
          /**
           * $ldap_user_conf->orphanedDrupalAcctBehavior will either be
           *  'ldap_user_orphan_email' or one of the user module options:
           *     user_cancel_block, user_cancel_block_unpublish,
           *     user_cancel_reassign, user_cancel_delete
           */
          if ($ldap_user_conf->orphanedDrupalAcctBehavior == 'ldap_user_orphan_email') {
            $email_list[] = $account->name . "," . $account->mail . "," . $base_url . "/user/$uid/edit";
          }
          else {
            _user_cancel(array(), $account, $ldap_user_conf->orphanedDrupalAcctBehavior);
          }
        }
      }
    }
  }

  if (count($email_list) > 0) {
    $site_email =  variable_get('site_mail', FALSE);
    $params = array('accounts' => $email_list);
    if ($site_email) {
      drupal_mail(
        'ldap_user',
        'orphaned_accounts',
        $site_email,
        language_default(),
        $params
      );
    }
  }

  return TRUE;
}
