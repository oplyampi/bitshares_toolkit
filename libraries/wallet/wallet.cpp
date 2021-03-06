#include <bts/wallet/wallet.hpp>
#include <bts/wallet/exceptions.hpp>
#include <bts/wallet/wallet_db.hpp>
#include <bts/wallet/config.hpp>
#include <bts/wallet/url.hpp>
#include <bts/utilities/key_conversion.hpp>
#include <bts/blockchain/time.hpp>
#include <bts/blockchain/exceptions.hpp>
#include <bts/blockchain/balance_operations.hpp>
#include <bts/blockchain/market_operations.hpp>
#include <bts/blockchain/proposal_operations.hpp>
#include <bts/blockchain/account_operations.hpp>
#include <bts/blockchain/asset_operations.hpp>
#include <fc/thread/thread.hpp>
#include <fc/crypto/base58.hpp>
#include <fc/filesystem.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>

#include <fc/io/json.hpp>
#include <iostream>
#include <sstream>

#include <algorithm>

#include <bts/bitcoin/bitcoin.hpp>
#include <bts/bitcoin/multibit.hpp>
#include <bts/bitcoin/electrum.hpp>
#include <bts/bitcoin/armory.hpp>
#include <bts/keyhotee/import_keyhotee_id.hpp>

namespace bts { namespace wallet {

   FC_REGISTER_EXCEPTIONS( (wallet_exception)
                           (invalid_password)
                           (login_required)
                           (no_such_wallet)
                           (wallet_already_exists) )

   namespace detail {

      class wallet_impl : public chain_observer
      {
         public:
             wallet_impl()
             {
                 _scanner_thread.reset( new fc::thread( "wallet_scanner") );
             }

             ~wallet_impl()
             {
                 try { if( _scanner_thread ) _scanner_thread->quit(); } catch( ... ) {}
             }

             wallet*                            self;
             wallet_db                          _wallet_db;
             chain_database_ptr                 _blockchain;
             path                               _data_directory;
             path                               _current_wallet_path;
             fc::sha512                         _wallet_password;
             fc::optional<fc::time_point>       _scheduled_lock_time;
             fc::future<void>                   _relocker_done;
             bool                               _use_deterministic_one_time_keys;
             bool                               _delegate_scanning_enabled;
             fc::future<void>                   _scan_in_progress;

             std::unique_ptr<fc::thread>        _scanner_thread;
             float                              _scan_progress;

             struct login_record
             {
                 fc::ecc::private_key key;
                 fc::time_point_sec insertion_time;
             };
             std::map<public_key_type, login_record>  _login_map;
             fc::future<void>                         _login_map_cleaner;
             const static short                       _login_cleaner_interval_seconds = 60;
             const static short                       _login_lifetime_seconds = 300;

             void reschedule_relocker();
             void relocker();

             fc::ecc::private_key create_one_time_key()
             {
                if( !_use_deterministic_one_time_keys )
                {
                   return fc::ecc::private_key::generate();
                }
                return _wallet_db.new_private_key( _wallet_password );
             }

            /**
             * This method is called anytime the blockchain state changes including
             * undo operations.
             */
            virtual void state_changed( const pending_chain_state_ptr& state )override
            {
               uint32_t last_unlocked_scanned_number = _wallet_db.get_property( last_unlocked_scanned_block_number).as<uint32_t>();
               if ( _blockchain->get_head_block_num() < last_unlocked_scanned_number )
               {
                  _wallet_db.set_property( last_unlocked_scanned_block_number, fc::variant( _blockchain->get_head_block_num() ) );
               }
            }

            /**
             *  This method is called anytime a block is applied to the chain.
             */
            virtual void block_applied( const block_summary& summary )override
            {
               if( self->is_open() && self->is_unlocked() && (_delegate_scanning_enabled || self->get_my_delegates( enabled_delegate_status ).empty() ) )
               {
                  const auto account_priv_keys = _wallet_db.get_account_private_keys( _wallet_password );
                  const auto now = blockchain::now();
                  scan_block( summary.block_data.block_num, account_priv_keys, now );
               }
            }

            void scan_market_transaction(
                    const market_transaction& trx,
                    uint32_t block_num,
                    const time_point_sec& block_time,
                    const time_point_sec& received_time
                    )
            { try {
                const auto bid_is_short = ( trx.bid_type == short_order );
                const auto bid_type_str = string( !bid_is_short ? "bid" : "short" );

                auto okey_bid = _wallet_db.lookup_key( trx.bid_owner ); 
                if( okey_bid && okey_bid->has_private_key() )
                {
                   auto bid_account_key = _wallet_db.lookup_key( okey_bid->account_address );

                   auto bal_rec = _blockchain->get_balance_record( withdraw_condition( withdraw_with_signature(trx.bid_owner), 
                                                                                       trx.bid_price.base_asset_id ).get_address() );
                   if( bal_rec )
                   {
                       //wlog( "BAL RECORD ${R}", ("R", bal_rec) );
                       _wallet_db.cache_balance( *bal_rec );
                   }

                   bal_rec = _blockchain->get_balance_record( withdraw_condition( withdraw_with_signature(trx.bid_owner), 
                                                                                  trx.ask_price.quote_asset_id ).get_address() );

                   if( bal_rec )
                   {
                       //wlog( "BAL RECORD ${R}", ("R", bal_rec) );
                       _wallet_db.cache_balance( *bal_rec );
                   }

                   /* What we paid */
                   auto out_entry = ledger_entry();
                   out_entry.from_account = okey_bid->public_key;
                   out_entry.amount = trx.bid_paid;
                   out_entry.memo = "fill ask @ " + _blockchain->to_pretty_price( trx.bid_price );

                   /* What we received */
                   auto in_entry = ledger_entry();
                   in_entry.from_account = okey_bid->public_key;
                   in_entry.to_account = !bid_is_short ? bid_account_key->public_key : okey_bid->public_key;
                   in_entry.amount = trx.bid_received;
                   in_entry.memo = "fill " + bid_type_str + " @ " + _blockchain->to_pretty_price( trx.bid_price );

                   std::stringstream id_ss;
                   id_ss << block_num << self->get_key_label( okey_bid->public_key ) << "0";

                   auto record = wallet_transaction_record();
                   record.record_id = fc::ripemd160::hash( id_ss.str() );
                   record.block_num = block_num;
                   record.is_virtual = record.is_market = true;
                   record.ledger_entries.push_back( out_entry );
                   record.ledger_entries.push_back( in_entry );
                   record.fee = trx.fees_collected;
                   record.created_time = block_time;
                   record.received_time = received_time;

                   _wallet_db.store_transaction( record );
                }

                auto okey_ask = _wallet_db.lookup_key( trx.ask_owner ); 
                if( okey_ask && okey_ask->has_private_key() )
                {
                   auto ask_account_key = _wallet_db.lookup_key( okey_ask->account_address );

                   auto bal_rec = _blockchain->get_balance_record( withdraw_condition( withdraw_with_signature(trx.ask_owner),
                                                                                       trx.ask_price.base_asset_id ).get_address() );
                   if( bal_rec )
                   {
                      //wlog( "ASK BAL RECORD ${R}", ("R", bal_rec) );
                      _wallet_db.cache_balance( *bal_rec );
                   }

                   bal_rec = _blockchain->get_balance_record( withdraw_condition( withdraw_with_signature(trx.ask_owner),
                                                                                  trx.ask_price.quote_asset_id ).get_address() );

                   if( bal_rec ) 
                   {
                      //wlog( "ASK BAL RECORD ${R}", ("R", bal_rec) );
                      _wallet_db.cache_balance( *bal_rec );
                   }

                   /* What we paid */
                   auto out_entry = ledger_entry();
                   out_entry.from_account = okey_ask->public_key;
                   out_entry.amount = trx.ask_paid;
                   out_entry.memo = "fill " + bid_type_str + " @ " + _blockchain->to_pretty_price( trx.ask_price );

                   /* What we received */
                   auto in_entry = ledger_entry();
                   in_entry.from_account = okey_ask->public_key;
                   in_entry.to_account = ask_account_key->public_key;
                   in_entry.amount = trx.ask_received;
                   in_entry.memo = "fill ask @ " + _blockchain->to_pretty_price( trx.ask_price );

                   std::stringstream id_ss;
                   id_ss << block_num << self->get_key_label( okey_ask->public_key ) << "1";

                   auto record = wallet_transaction_record();
                   record.record_id = fc::ripemd160::hash( id_ss.str() );
                   record.block_num = block_num;
                   record.is_virtual = record.is_market = true;
                   record.ledger_entries.push_back( out_entry );
                   record.ledger_entries.push_back( in_entry );
                   record.fee = trx.fees_collected;
                   record.created_time = block_time;
                   record.received_time = received_time;

                   _wallet_db.store_transaction( record );
                }
            } FC_CAPTURE_AND_RETHROW() }

            secret_hash_type get_secret( uint32_t block_num,
                                         const private_key_type& delegate_key )const;

            void scan_block( uint32_t block_num, const vector<private_key_type>& keys, const time_point_sec& received_time );

            void scan_transaction( const signed_transaction& transaction, uint32_t block_num, const time_point_sec& block_timestamp,
                                   const vector<private_key_type>& keys, const time_point_sec& received_time );

            bool scan_withdraw( const withdraw_operation& op, wallet_transaction_record& trx_rec );

            bool scan_deposit( const deposit_operation& op, const private_keys& keys, wallet_transaction_record& trx_rec );

            bool scan_register_account( const register_account_operation& op );
            bool scan_update_account( const update_account_operation& op );
            bool scan_create_asset( wallet_transaction_record& trx_rec, const create_asset_operation& op );
            bool scan_issue_asset( wallet_transaction_record& trx_rec, const issue_asset_operation& op );
            bool scan_bid( wallet_transaction_record& trx_rec, const bid_operation& op );
            bool scan_ask( wallet_transaction_record& trx_rec, const ask_operation& op );
            bool scan_short( wallet_transaction_record& trx_rec, const short_operation& op );

            bool sync_balance_with_blockchain( const balance_id_type& balance_id );

            vector<wallet_transaction_record> get_pending_transactions()const;
            void hide_pending_transactions();

            void scan_balances( const time_point_sec& received_time );
            void scan_registered_accounts();
            void withdraw_to_transaction( share_type amount,
                                          asset_id_type asset_id,
                                          const address& from_account_address,
                                          signed_transaction& trx, 
                                          unordered_set<address>& required_fees );
            bool address_in_account( const address& address_to_check,
                                     const address& account_address )const;

            owallet_transaction_record lookup_transaction( const transaction_id_type& trx_id )const;
      };
     
      vector<wallet_transaction_record> wallet_impl::get_pending_transactions()const
      {
          return _wallet_db.get_pending_transactions();
      }

      void wallet_impl::hide_pending_transactions()
      {
          _wallet_db.hide_pending_transactions();
      }

      void wallet_impl::scan_balances( const time_point_sec& received_time )
      {
         /* Delete ledger entries for any genesis balances before we can reconstruct them */
          /*
         const auto my_accounts = self->list_my_accounts();
         for( const auto& account : my_accounts )
         {
             const auto record_id = fc::ripemd160::hash( account.name );
             auto transaction_record = _wallet_db.lookup_transaction( record_id );
             if( transaction_record.valid() )
             {
                 transaction_record->ledger_entries.clear();
                 _wallet_db.store_transaction( *transaction_record );
             }
         }
         */

         _blockchain->scan_balances( [&]( const balance_record& bal_rec )
         {
              const auto key_rec = _wallet_db.lookup_key( bal_rec.owner() );
              if( key_rec.valid() && key_rec->has_private_key() )
              {
                _wallet_db.cache_balance( bal_rec );

                if( bal_rec.genesis_info.valid() ) /* Create virtual transactions for genesis claims */
                {
                    const auto public_key = key_rec->public_key;
                    //const auto record_id = fc::ripemd160::hash( self->get_key_label( public_key ) );
                    const auto record_id = bal_rec.id().addr;
                    auto transaction_record = _wallet_db.lookup_transaction( record_id );
                    if( !transaction_record.valid() )
                    {
                        transaction_record = wallet_transaction_record();
                        transaction_record->record_id = record_id;
                        transaction_record->is_virtual = true;
                        transaction_record->created_time = _blockchain->get_genesis_timestamp();
                        transaction_record->received_time = received_time;

                    auto entry = ledger_entry();
                    entry.to_account = public_key;
                    //entry.amount = bal_rec.genesis_info->initial_balance;
                    entry.amount = bal_rec.get_balance();
                    entry.memo = "claim " + bal_rec.genesis_info->claim_addr;

                    transaction_record->ledger_entries.push_back( entry );
                    _wallet_db.store_transaction( *transaction_record );
                    }

                }
              }
         } );
      }

      void wallet_impl::scan_registered_accounts()
      {
         _blockchain->scan_accounts( [&]( const blockchain::account_record& scanned_account_record )
         {
              // TODO: check owner key as well!
              auto key_rec =_wallet_db.lookup_key( scanned_account_record.active_key() );
              if( key_rec.valid() && key_rec->has_private_key() )
              {
                 auto existing_account_record = _wallet_db.lookup_account( key_rec->account_address );
                 if( existing_account_record.valid() )
                 {
                    account_record& blockchain_account_record = *existing_account_record;
                    blockchain_account_record = scanned_account_record;
                    _wallet_db.cache_account( *existing_account_record );
                 }
              }
         } );
         ilog( "account scan complete" );
      }

      bool wallet_impl::address_in_account( const address& address_to_check, 
                                            const address& account_address )const
      { try {
         if( address_to_check == account_address )
            return true;
         auto opt_key = _wallet_db.lookup_key( address_to_check );
         if( !opt_key.valid() )
            return false;
         return opt_key->account_address == account_address;
      } FC_RETHROW_EXCEPTIONS( warn, "", ("address_to_check",address_to_check)("account_address",account_address) ) }

      void wallet_impl::withdraw_to_transaction( share_type amount,
                                                 asset_id_type asset_id,
                                                 const address& from_account_address,
                                                 signed_transaction& trx, 
                                                 unordered_set<address>& required_signatures )
      { try {
         auto pending_state = _blockchain->get_pending_state();
         share_type remaining = amount;
         for( const auto& balance_item : _wallet_db.get_balances() )
         {
            auto owner = balance_item.second.owner();
            //auto oaccount = _wallet_db.lookup_account( owner );
            //string name = oaccount ? oaccount->name : string(owner);
            //ilog( "${name} owner ${owner} == ${from_account}  ${item}", ("name",name)("owner",owner)("from_account",from_account_address)("item",balance_item) );

            if( balance_item.second.asset_id() == asset_id && 
                address_in_account( owner, from_account_address ) )
            {
               auto current_balance = pending_state->get_balance_record( balance_item.first );
               if( current_balance )
               {
                  if( current_balance->balance > 0 )
                  {
                     if( remaining > current_balance->balance )
                     {
                        trx.withdraw( balance_item.first, balance_item.second.balance );
                        remaining -= current_balance->balance;
                     //   balance_item.second.balance = 0;
                        required_signatures.insert( balance_item.second.owner() );
                     //   _wallet_db.cache_balance( balance_item.second );
                     }
                     else
                     {
                        trx.withdraw( balance_item.first, remaining );
                      //  balance_item.second.balance -= remaining;
                        remaining = 0;
                     //   _wallet_db.cache_balance( balance_item.second );
                        required_signatures.insert( current_balance->owner() );
                        return;
                     }
                 }
               }
            }
         }
         auto required = _blockchain->to_pretty_asset( asset(amount,asset_id) );
         auto available = _blockchain->to_pretty_asset( asset(amount-remaining,asset_id) );
         FC_CAPTURE_AND_THROW( insufficient_funds, (required)(available) );
      } FC_CAPTURE_AND_RETHROW( (amount)(asset_id)(from_account_address)(trx)(required_signatures) ) }

      secret_hash_type wallet_impl::get_secret( uint32_t block_num, 
                                                const private_key_type& delegate_key )const
      {
         block_id_type header_id;
         if( block_num != uint32_t(-1) && block_num > 1 )
         {
            auto block_header = _blockchain->get_block_header( block_num - 1 );
            header_id = block_header.id();
         }

         fc::sha512::encoder key_enc;
         fc::raw::pack( key_enc, delegate_key );
         fc::sha512::encoder enc;
         fc::raw::pack( enc, key_enc.result() );
         fc::raw::pack( enc, header_id );

         return fc::ripemd160::hash( enc.result() );
      }

      void wallet_impl::scan_block( uint32_t block_num, const private_keys& keys, const time_point_sec& received_time )
      {
         const auto block = _blockchain->get_block( block_num );
         for( const auto& transaction : block.user_transactions )
            scan_transaction( transaction, block_num, block.timestamp, keys, received_time );

         const auto market_trxs = _blockchain->get_market_transactions( block_num );
         for( const auto& market_trx : market_trxs )
            scan_market_transaction( market_trx, block_num, block.timestamp, received_time );
      }

      void wallet_impl::scan_transaction( const signed_transaction& transaction, uint32_t block_num, const time_point_sec& block_timestamp,
                                          const vector<private_key_type>& keys, const time_point_sec& received_time )
      {
          const auto record_id = transaction.id();
          auto transaction_record = _wallet_db.lookup_transaction( record_id );
          auto is_duplicate = transaction_record.valid();
          if( !is_duplicate ) /* If new transaction */
          {
              transaction_record = wallet_transaction_record();
              transaction_record->record_id = record_id;
              transaction_record->trx = transaction;
              transaction_record->created_time = block_timestamp;
              transaction_record->received_time = received_time;
          }

          transaction_record->block_num = block_num;
          transaction_record->is_confirmed = true;
          //transaction_record->ledger_entries.clear(); /* Reconstruct ledger entries each time; sorry memo */
          if( is_duplicate ) _wallet_db.store_transaction( *transaction_record );

          auto store_record = is_duplicate;
          for( const auto& op : transaction.operations )
          {
              switch( operation_type_enum( op.type ) )
              {
                  case null_op_type:
                      FC_THROW_EXCEPTION( invalid_operation, "Null operation type!", ("op",op) );
                      break;

                  case withdraw_op_type:
                      store_record |= scan_withdraw( op.as<withdraw_operation>(), *transaction_record );
                      break;
                  case deposit_op_type:
                      store_record |= scan_deposit( op.as<deposit_operation>(), keys, *transaction_record );
                      break;

                  case register_account_op_type:
                      store_record |= scan_register_account( op.as<register_account_operation>() );
                      break;
                  case update_account_op_type:
                      store_record |= scan_update_account( op.as<update_account_operation>() );
                      break;
                  case withdraw_pay_op_type:
                      // TODO: FC_THROW( "withdraw_pay_op_type not implemented!" );
                      break;

                  case create_asset_op_type:
                      store_record |= scan_create_asset( *transaction_record, op.as<create_asset_operation>() );
                      break;
                  case update_asset_op_type:
                      // TODO: FC_THROW( "update_asset_op_type not implemented!" );
                      break;
                  case issue_asset_op_type:
                      store_record |= scan_issue_asset( *transaction_record, op.as<issue_asset_operation>() );
                      break;

                  case fire_delegate_op_type:
                      // TODO: FC_THROW( "fire_delegate_op_type not implemented!" );
                      break;

                  case submit_proposal_op_type:
                      // TODO: FC_THROW( "submit_proposal_op_type not implemented!" );
                      break;
                  case vote_proposal_op_type:
                      // TODO: FC_THROW( "vote_proposal_op_type not implemented!" );
                      break;

                  case bid_op_type:
                      store_record |= scan_bid( *transaction_record, op.as<bid_operation>() );
                      break;
                  case ask_op_type:
                      store_record |= scan_ask( *transaction_record, op.as<ask_operation>() );
                      break;
                  case short_op_type:
                      store_record |= scan_short( *transaction_record, op.as<short_operation>() );
                      break;
                  case cover_op_type:
                      // TODO: FC_THROW( "cover_op_type not implemented!" );
                      break;
                  case add_collateral_op_type:
                      // TODO: FC_THROW( "add_collateral_op_type not implemented!" );
                      break;
                  case remove_collateral_op_type:
                      // TODO: FC_THROW( "remove_collateral_op_type not implemented!" );
                      break;

                  case define_delegate_slate_op_type:
                      // TODO: FC_THROW( "remove_collateral_op_type not implemented!" );
                      break;

                  default:
                      FC_THROW_EXCEPTION( invalid_operation, "Unknown operation type!", ("op",op) );
                      break;
              }
          }

          // TODO: link matching withdraws and deposits, but do it in to_pretty_trx
          if( store_record && !is_duplicate ) _wallet_db.store_transaction( *transaction_record );
      }

      bool wallet_impl::scan_withdraw( const withdraw_operation& op, wallet_transaction_record& trx_rec )
      {
         auto bal_rec = _wallet_db.lookup_balance( op.balance_id );
         if( bal_rec.valid() )
         {
            // TODO: Only if withdraw by signature or by name
            /*
            const auto key_rec =_wallet_db.lookup_key( bal_rec->owner() );

            auto entry = ledger_entry();
            if( key_rec.valid() ) entry.from_account = key_rec->public_key;
            entry.amount = asset( op.amount, bal_rec->condition.asset_id );

            trx_rec.ledger_entries.push_back( entry );
            */
            sync_balance_with_blockchain( op.balance_id );

            return true;
         }
         return false;
      }

      bool wallet_impl::scan_register_account( const register_account_operation& op )
      {
          auto opt_key_rec = _wallet_db.lookup_key( op.owner_key );

          if( !opt_key_rec.valid() ) 
             return false;

          auto opt_account = _wallet_db.lookup_account( address( op.owner_key ) );
          if( !opt_account.valid() )
          {
             wlog( "We have the key but no account for registration operation" );
             return false;
          }

          wlog( "we detected an account register operation for ${name}", ("name",op.name) );
          auto account_name_rec = _blockchain->get_account_record( op.name );
          FC_ASSERT( account_name_rec.valid() );

          blockchain::account_record& tmp = *opt_account;
          tmp = *account_name_rec;
          _wallet_db.cache_account( *opt_account );

          return false;
      }

      bool wallet_impl::scan_update_account( const update_account_operation& op )
      { try {
          auto oaccount =  _blockchain->get_account_record( op.account_id ); 
          FC_ASSERT( oaccount.valid() );
          auto opt_key_rec = _wallet_db.lookup_key( oaccount->owner_key );
          if( !opt_key_rec.valid() ) 
             return false;

          auto opt_account = _wallet_db.lookup_account( address( oaccount->owner_key ) );
          if( !opt_account.valid() )
          {
             wlog( "We have the key but no account for registration operation" );
             return false;
          }
          wlog( "we detected an account register operation for ${name}", ("name",oaccount->name) );
          auto account_name_rec = _blockchain->get_account_record( oaccount->name );
          FC_ASSERT( account_name_rec.valid() );

          blockchain::account_record& tmp = *opt_account;
          tmp = *account_name_rec; 
          _wallet_db.cache_account( *opt_account );

          return false;
      } FC_RETHROW_EXCEPTIONS( warn, "", ("op",op) ) }

      bool wallet_impl::scan_create_asset( wallet_transaction_record& trx_rec, 
                                           const create_asset_operation& op )
      {
         //wlog( "${op}", ("op",op) );
         if( op.issuer_account_id != asset_record::market_issued_asset )
         {
            auto oissuer =  _blockchain->get_account_record( op.issuer_account_id );
            FC_ASSERT( oissuer.valid() );
            auto opt_key_rec = _wallet_db.lookup_key( oissuer->owner_key );
            if( opt_key_rec.valid() && opt_key_rec->has_private_key() )
            {
               auto entry = ledger_entry();
               entry.from_account = oissuer->owner_key;
               entry.to_account = oissuer->owner_key;
               entry.memo = "create " + op.symbol + " (" + op.name + ")";
               trx_rec.ledger_entries.push_back( entry );
               return true;
            }
         }
         return false;
      }

      bool wallet_impl::scan_issue_asset( wallet_transaction_record& trx_rec, 
                                          const issue_asset_operation& op )
      {
         //wlog( "${op}", ("op",op) );
         auto asset_rec = _blockchain->get_asset_record( op.amount.asset_id );
         if( asset_rec.valid() )
         {
             auto entry = ledger_entry();
             auto issuer = _blockchain->get_account_record( asset_rec->issuer_account_id );
             if( issuer.valid() ) entry.from_account = issuer->active_key();
             entry.memo = "issue " + _blockchain->to_pretty_asset( op.amount );
             trx_rec.ledger_entries.push_back( entry );
             // TODO: Return true here if this wallet contains the issuer
         }
         return false;
      }

      bool wallet_impl::scan_bid( wallet_transaction_record& trx_rec, const bid_operation& bid_op )
      { try {
          auto okey_rec = _wallet_db.lookup_key( bid_op.bid_index.owner ); 
          if( okey_rec && okey_rec->has_private_key() )
          {
             auto order = _blockchain->get_market_bid( bid_op.bid_index );
             //FC_ASSERT( order.valid() );
             _wallet_db.update_market_order( bid_op.bid_index.owner, order, trx_rec.trx.id() );
             return true;
          }
          return false;
      } FC_CAPTURE_AND_RETHROW( (bid_op) ) } 

      bool wallet_impl::scan_ask( wallet_transaction_record& trx_rec, const ask_operation& ask_op )
      { try {
          auto okey_rec = _wallet_db.lookup_key( ask_op.ask_index.owner ); 
          if( okey_rec && okey_rec->has_private_key() )
          {
             auto order = _blockchain->get_market_ask( ask_op.ask_index );
             _wallet_db.update_market_order( ask_op.ask_index.owner, order, trx_rec.trx.id() );
             return true;
          }
          return false;
      } FC_CAPTURE_AND_RETHROW( (ask_op) ) } 

      bool wallet_impl::scan_short( wallet_transaction_record& trx_rec, const short_operation& short_op )
      { try {
          auto okey_rec = _wallet_db.lookup_key( short_op.short_index.owner ); 
          if( okey_rec && okey_rec->has_private_key() )
          {
             auto order = _blockchain->get_market_short( short_op.short_index );
             _wallet_db.update_market_order( short_op.short_index.owner, order, trx_rec.trx.id() );
             return true;
          }
          return false;
      } FC_CAPTURE_AND_RETHROW( (short_op) ) } 

      bool wallet_impl::scan_deposit( const deposit_operation& op, const private_keys& keys,
                                      wallet_transaction_record& trx_rec )
      { try {
          bool cache_deposit = false; 
          switch( (withdraw_condition_types) op.condition.type )
          {
             case withdraw_null_type:
             {
                FC_THROW( "withdraw_null_type not implemented!" );
                break;
             }
             case withdraw_signature_type:
             {
                auto deposit = op.condition.as<withdraw_with_signature>();
                if( _wallet_db.has_private_key( deposit.owner ) )
                {
                   cache_deposit = true;
                }
                else if( deposit.memo )
                {
                   _scanner_thread->async( [&]()
                   {
                       for( const auto& key : keys )
                       {
                           // TODO: see how we can optimize this
                          omemo_status status = deposit.decrypt_memo_data( key );
                          if( status.valid() )
                          {
                             _wallet_db.cache_memo( *status, key, _wallet_password );
                             auto entry = ledger_entry();
                             if( status->memo_flags == from_memo )
                             {
                                entry.from_account = status->from;
                                entry.to_account = key.get_public_key();
                                entry.amount = asset( op.amount, op.condition.asset_id );
                                entry.memo = status->get_message();
                                //ilog( "FROM MEMO... ${msg}", ("msg",trx_rec.memo_message) );
                             }
                             else
                             {
                                //ilog( "TO MEMO OLD STATE: ${s}",("s",trx_rec) );
                                //ilog( "op: ${op}", ("op",op) );
                                entry.from_account = key.get_public_key();
                                entry.to_account = status->from;
                                entry.memo = status->get_message();
                                //ilog( "TO MEMO NEW STATE: ${s}",("s",trx_rec) );
                             }
                             trx_rec.ledger_entries.push_back( entry );
                             cache_deposit = true;
                             break;
                          }
                       }
                   } ).wait();
                   break;
                }
                break;
             }
             case withdraw_multi_sig_type:
             {
                // TODO: FC_THROW( "withdraw_multi_sig_type not implemented!" );
                break;
             }
             case withdraw_password_type:
             {
                // TODO: FC_THROW( "withdraw_password_type not implemented!" );
                break;
             }
             case withdraw_option_type:
             {
                // TODO: FC_THROW( "withdraw_option_type not implemented!" );
                break;
             }
             default:
             {
                FC_THROW( "unknown withdraw condition type!" );
                break;
             }
        }
        if( cache_deposit )
        {
           if( !sync_balance_with_blockchain( op.balance_id() ) )
           {
              elog( "unable to cache balance ${b}", ("b",op) );
           }
        }
        return cache_deposit;

      } FC_RETHROW_EXCEPTIONS( warn, "", ("op",op) ) } // wallet_impl::scan_deposit 

      bool wallet_impl::sync_balance_with_blockchain( const balance_id_type& balance_id )
      {
         auto bal_rec = _blockchain->get_balance_record( balance_id );
         if( !bal_rec.valid() )
         {
            // wlog( "blockchain doesn't know about balance id: ${balance_id}",
            //      ("balance_id",balance_id) );
            _wallet_db.remove_balance( balance_id );
            return false;
         }
         else
         {
            _wallet_db.cache_balance( *bal_rec );
            return true;
         }
      }

      void wallet_impl::reschedule_relocker()
      {
        if( !_relocker_done.valid() || _relocker_done.ready() )
          _relocker_done = fc::async( [this](){ relocker(); } );
      }

      void wallet_impl::relocker()
      {
          fc::time_point now = fc::time_point::now();
          ilog( "Starting wallet relocker task at time: ${t}", ("t", now) );
          if( !_scheduled_lock_time || now >= *_scheduled_lock_time )
          {
              self->lock();
          }
          else
          {
              ilog( "Scheduling wallet relocker task for time: ${t}", ("t", *_scheduled_lock_time) );
              _relocker_done = fc::schedule( [this](){ relocker(); }, *_scheduled_lock_time );
          }
      }

   } // detail 

   wallet::wallet( chain_database_ptr blockchain )
   :my( new detail::wallet_impl() )
   {
      my->self = this;
      my->_delegate_scanning_enabled = false;
      my->_use_deterministic_one_time_keys = true;
      my->_blockchain = blockchain;
      my->_blockchain->add_observer( my.get() );
   }

   wallet::~wallet()
   {
      close();
   }
 
   void wallet::use_deterministic_one_time_keys( bool state )
   {
      my->_use_deterministic_one_time_keys = state;
   }

   void wallet::set_data_directory( const path& data_dir )
   {
      my->_data_directory = data_dir;
   }

   path wallet::get_data_directory()const
   {
      return my->_data_directory;
   }

   void wallet::create( const string& wallet_name, 
                        const string& password,
                        const string& brainkey )
   { try {
      if( !is_valid_account_name( wallet_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid name for a wallet!", ("wallet_name",wallet_name) );

      auto wallet_file_path = fc::absolute( get_data_directory() ) / wallet_name;
      if( fc::exists( wallet_file_path ) )
          FC_THROW_EXCEPTION( wallet_already_exists, "Wallet name already exists!", ("wallet_name",wallet_name) );

      if( password.size() < BTS_WALLET_MIN_PASSWORD_LENGTH )
          FC_THROW_EXCEPTION( password_too_short, "Password too short!", ("size",password.size()) );

      try
      {
          create_file( wallet_file_path, password, brainkey );
          open( wallet_name );
          unlock( password, BTS_WALLET_DEFAULT_UNLOCK_TIME_SEC );
      }
      catch( ... )
      {
          close();
          throw;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to create wallet '${wallet_name}'", ("wallet_name",wallet_name) ) }

   void wallet::create_file( const path& wallet_file_path,
                             const string& password,
                             const string& brainkey )
   { try {
      if( fc::exists( wallet_file_path ) )
          FC_THROW_EXCEPTION( wallet_already_exists, "Wallet file already exists!", ("wallet_file_path",wallet_file_path) );

      if( password.size() < BTS_WALLET_MIN_PASSWORD_LENGTH )
          FC_THROW_EXCEPTION( password_too_short, "Password too short!", ("size",password.size()) );

      try
      {
          close();

          my->_wallet_db.open( wallet_file_path );
          my->_wallet_password = fc::sha512::hash( password.c_str(), password.size() );

          master_key new_master_key;
          extended_private_key epk;
          if( !brainkey.empty() )
          {
             auto base = fc::sha512::hash( brainkey.c_str(), brainkey.size() );

             /* strengthen the key a bit */
             for( uint32_t i = 0; i < 100ll*1000ll; ++i )
                base = fc::sha512::hash( base );

             epk = extended_private_key( base );
          }
          else
          {
             wlog( "generating random" );
             epk = extended_private_key( private_key_type::generate() );
          }

          my->_wallet_db.set_master_key( epk, my->_wallet_password);

          my->_wallet_db.set_property( last_unlocked_scanned_block_number, variant( my->_blockchain->get_head_block_num() ) );
          my->_wallet_db.set_property( default_transaction_priority_fee, variant( asset( BTS_BLOCKCHAIN_DEFAULT_PRIORITY_FEE ) ) );

          my->_wallet_db.close();
          my->_wallet_db.open( wallet_file_path );
          my->_current_wallet_path = wallet_file_path;


          FC_ASSERT( my->_wallet_db.validate_password( my->_wallet_password ) );
      }
      catch( ... )
      {
          close();
          fc::remove_all( wallet_file_path );
          throw;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to create wallet '${wallet_file_path}'", ("wallet_file_path",wallet_file_path) ) }

   void wallet::open( const string& wallet_name )
   { try {
      if( !is_valid_account_name( wallet_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid name for a wallet!", ("wallet_name",wallet_name) );

      auto wallet_file_path = fc::absolute( get_data_directory() ) / wallet_name;
      if ( !fc::exists( wallet_file_path ) )
         FC_THROW_EXCEPTION( no_such_wallet, "No such wallet exists!", ("wallet_name", wallet_name) );

      try
      {
          open_file( wallet_file_path );
      }
      catch( ... )
      {
          close();
          throw;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("wallet_name",wallet_name) ) }

   void wallet::open_file( const path& wallet_file_path )
   { try {
      if ( !fc::exists( wallet_file_path ) )
         FC_THROW_EXCEPTION( no_such_wallet, "No such wallet exists!", ("wallet_file_path", wallet_file_path) );

      try
      {
          close();
          my->_wallet_db.open( wallet_file_path );
          my->_current_wallet_path = wallet_file_path;

          const auto tmp_balances = my->_wallet_db.get_balances();
          for( const auto& item : tmp_balances )
              my->sync_balance_with_blockchain( item.first );

          my->_blockchain->set_priority_fee( get_priority_fee().amount );
      }
      catch( ... )
      {
          close();
          throw;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "Unable to open wallet ${wallet_file_path}", ("wallet_file_path",wallet_file_path) ) }

   void wallet::close()
   { try {
      if( my->_scan_in_progress.valid() )
      {
         my->_scan_in_progress.cancel();
         try {
            my->_scan_in_progress.wait();
         } catch ( ... ) {}
      }
      lock();
      ilog( "Canceling wallet relocker task..." );
      try { my->_relocker_done.cancel_and_wait(); } catch( ... ) {}
      ilog( "Wallet relocker task canceled" );
      my->_wallet_db.close();
      my->_current_wallet_path = fc::path();
      my->_use_deterministic_one_time_keys = false;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   bool wallet::is_open()const
   {
      return my->_wallet_db.is_open();
   }

   string wallet::get_wallet_name()const
   {
      return my->_current_wallet_path.filename().generic_string();
   }

   path wallet::get_wallet_filename()const
   {
      return my->_current_wallet_path;
   }

   void wallet::export_to_json( const path& filename )const
   { try {
      if( fc::exists( filename ) )
          FC_THROW_EXCEPTION( file_already_exists, "Filename to export to already exists!", ("filename",filename) );

      FC_ASSERT( is_open() );
      my->_wallet_db.export_to_json( filename );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("filename",filename) ) }

   void wallet::create_from_json( const path& filename, const string& wallet_name, const string& passphrase )
   { try {
      if( !fc::exists( filename ) )
          FC_THROW_EXCEPTION( file_not_found, "Filename to import from could not be found!", ("filename",filename) );

      if( !is_valid_account_name( wallet_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid name for a wallet!", ("wallet_name",wallet_name) );

      try
      {
          create( wallet_name, passphrase );
          my->_wallet_db.import_from_json( filename );
          unlock( passphrase, BTS_WALLET_DEFAULT_UNLOCK_TIME_SEC );
      }
      catch( ... )
      {
          close();
          auto wallet_file_path = fc::absolute( get_data_directory() ) / wallet_name;
          fc::remove_all( wallet_file_path );
          throw;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("filename",filename)("wallet_name",wallet_name) ) }

   void wallet::unlock( const string& password, uint32_t timeout_seconds )
   { try {
      try
      {
          FC_ASSERT( is_open() );

          if( timeout_seconds < 1 )
              FC_THROW_EXCEPTION( invalid_timeout, "Invalid timeout!" );

          const auto now = fc::time_point::now();
          const auto new_lock_time = now + fc::seconds( timeout_seconds );
          if( new_lock_time.sec_since_epoch() <= now.sec_since_epoch() )
              FC_THROW_EXCEPTION( invalid_timeout, "Invalid timeout!" );

          if( password.size() < BTS_WALLET_MIN_PASSWORD_LENGTH )
              FC_THROW_EXCEPTION( password_too_short, "Invalid password!" );

          my->_wallet_password = fc::sha512::hash( password.c_str(), password.size() );
          if( !my->_wallet_db.validate_password( my->_wallet_password ) )
              FC_THROW_EXCEPTION( invalid_password, "Invalid password!" );

          my->_scheduled_lock_time = new_lock_time;
          ilog( "Wallet unlocked at time: ${t}", ("t", fc::time_point_sec(now)) );
          my->reschedule_relocker();
          wallet_lock_state_changed( false );
          ilog( "Wallet unlocked until time: ${t}", ("t", fc::time_point_sec(*my->_scheduled_lock_time)) );

          /* Scan blocks we have missed while locked */
          uint32_t first = my->_wallet_db.get_property( last_unlocked_scanned_block_number).as<uint32_t>();
          scan_chain( first,
                      my->_blockchain->get_head_block_num(),
                      [first](uint32_t current, uint32_t end){
              std::cout << " Scanning for new transactions in block: " << current-first << '/' << end-first << "\r" << std::flush;
          });
          std::cout << "Finished scanning for new transactions.                                " << std::endl;
      }
      catch( ... )
      {
          lock();
          throw;
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("timeout_seconds", timeout_seconds) ) }

   void wallet::lock()
   {
      try { my->_login_map_cleaner.cancel_and_wait(); } catch( ... ) {}
      my->_wallet_password     = fc::sha512();
      my->_scheduled_lock_time = fc::optional<fc::time_point>();
      wallet_lock_state_changed( true );
      ilog( "Wallet locked at time: ${t}", ("t",blockchain::now()) );
   }

   void wallet::change_passphrase( const string& new_passphrase )
   { try {
      if( NOT is_open() ) FC_CAPTURE_AND_THROW( wallet_closed );
      if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );
      if( new_passphrase.size() < BTS_WALLET_MIN_PASSWORD_LENGTH ) FC_CAPTURE_AND_THROW( password_too_short );

      auto new_password = fc::sha512::hash( new_passphrase.c_str(), new_passphrase.size() );

      my->_wallet_db.change_password( my->_wallet_password, new_password );
      my->_wallet_password = new_password;
   } FC_CAPTURE_AND_RETHROW() }

   bool wallet::is_unlocked()const
   {
      FC_ASSERT( is_open() );
      return !wallet::is_locked();
   }   

   bool wallet::is_locked()const
   {
      FC_ASSERT( is_open() );
      return my->_wallet_password == fc::sha512();
   }

   fc::optional<fc::time_point_sec> wallet::unlocked_until()const
   {
      FC_ASSERT( is_open() );
      return my->_scheduled_lock_time ? *my->_scheduled_lock_time : fc::optional<fc::time_point_sec>();
   }

   void wallet::set_setting(const string& name, const variant& value)
   {
       my->_wallet_db.store_setting(name, value);
   }

   fc::optional<variant> wallet::get_setting(const string& name)
   {
       return my->_wallet_db.lookup_setting(name);
   }

   public_key_type  wallet::create_account( const string& account_name, 
                                            const variant& private_data )
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( !current_account.valid(), "This name is already in your wallet." );

      auto existing_registered_account = my->_blockchain->get_account_record( account_name );
      FC_ASSERT( !existing_registered_account.valid(), "This name is already registered with the blockchain." );

      auto new_priv_key = my->_wallet_db.new_private_key( my->_wallet_password );
      auto new_pub_key  = new_priv_key.get_public_key();

      my->_wallet_db.add_account( account_name, new_pub_key, private_data );

      return new_pub_key;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   void wallet::account_set_favorite( const string& account_name,
                                      const bool is_favorite )
   {
       FC_ASSERT( is_open() );
       get_account( account_name ); /* Just to check input */

       auto judged_account = my->_wallet_db.lookup_account( account_name );
       judged_account->is_favorite = is_favorite;
       my->_wallet_db.cache_account( *judged_account );
   }

   /**
    *  Creates a new private key under the specified account. This key
    *  will not be valid for sending TITAN transactions to, but will
    *  be able to receive payments directly.
    */
   address  wallet::get_new_address( const string& account_name )
   { try {
      if( NOT is_open() ) FC_CAPTURE_AND_THROW( wallet_closed );
      if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );
      if( NOT is_receive_account(account_name) )
          FC_CAPTURE_AND_THROW( unknown_receive_account, (account_name) );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( current_account.valid() );

      auto new_priv_key = my->_wallet_db.new_private_key( my->_wallet_password, 
                                                          current_account->account_address );
      return new_priv_key.get_public_key();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   /**
    *  Creates a new private key under the specified account. This key
    *  will not be valid for sending TITAN transactions to, but will
    *  be able to receive payments directly.
    */
   public_key_type  wallet::get_new_public_key( const string& account_name )
   { try {
      if( NOT is_open() ) FC_CAPTURE_AND_THROW( wallet_closed );
      if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );
      if( NOT is_receive_account(account_name) )
          FC_CAPTURE_AND_THROW( unknown_receive_account, (account_name) );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( current_account.valid() );

      auto new_priv_key = my->_wallet_db.new_private_key( my->_wallet_password, 
                                                          current_account->account_address );
      return new_priv_key.get_public_key();
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   /**
    *  A contact is an account for which this wallet does not have the private
    *  key.   If account_name is globally registered then this call will assume
    *  it is the same account and will fail if the key is not the same.
    *
    *  @param account_name - the name the account is known by to this wallet
    *  @param key - the public key that will be used for sending TITAN transactions
    *               to the account.
    */
   void  wallet::add_contact_account( const string& account_name, 
                                      const public_key_type& key,
                                      const variant& private_data )
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      FC_ASSERT( is_open() );
      idump( (account_name) );

      auto current_registered_account = my->_blockchain->get_account_record( account_name );

      if( current_registered_account.valid() && current_registered_account->active_key() != key )
         FC_ASSERT( false, "Account name is already registered under a different key" );

      auto current_account = my->_wallet_db.lookup_account( account_name );
      if( current_account.valid() )
      {
         wlog( "current account is valid... ${name}", ("name", *current_account) );
         FC_ASSERT( current_account->account_address == address(key),
                    "Account with ${name} already exists", ("name",account_name) );
         if( !private_data.is_null() )
            current_account->private_data = private_data;
         idump( (*current_account) );
         my->_wallet_db.cache_account( *current_account );
         return;
      }
      else
      {
         auto account_key = my->_wallet_db.lookup_key( address(key) );
         FC_ASSERT( !account_key.valid(), "Provided key belongs to another account." );
         if( current_registered_account.valid() )
         {
            my->_wallet_db.add_account( *current_registered_account, private_data );
         }
         else
         {
            my->_wallet_db.add_account( account_name, key, private_data );
         }
         account_key = my->_wallet_db.lookup_key( address(key) );
         FC_ASSERT( account_key.valid() );
      }

   } FC_CAPTURE_AND_RETHROW( (account_name)(key) ) } 

   // TODO: This function is sometimes used purely for error checking of the account_name; refactor
   wallet_account_record wallet::get_account( const string& account_name )const
   { try {
      FC_ASSERT( is_open() );

      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      auto local_account = my->_wallet_db.lookup_account( account_name );
      if( !local_account.valid() )
          FC_THROW_EXCEPTION( unknown_account, "Unknown local account name!", ("account_name",account_name) );

      auto chain_account = my->_blockchain->get_account_record( account_name );
      if( chain_account )
      {
         if( local_account->owner_key == chain_account->owner_key )
         {
             blockchain::account_record& bca = *local_account;
             bca = *chain_account;
             my->_wallet_db.cache_account( *local_account );
         }
         else
         {
            wlog( "local account is owned by someone different public key than blockchain account" );
            wdump( (local_account)(chain_account) );
         }
      }
      return *local_account;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::remove_contact_account( const string& account_name )
   { try {
      if( !is_valid_account( account_name ) )
          FC_THROW_EXCEPTION( unknown_account, "Unknown account name!", ("account_name",account_name) );

      if( !is_unique_account(account_name) )
          FC_THROW_EXCEPTION( duplicate_account_name,
                              "Local account name conflicts with registered name. Please rename your local account first.", ("account_name",account_name) );

      const auto oaccount = my->_wallet_db.lookup_account( account_name );
      if( my->_wallet_db.has_private_key( address( oaccount->owner_key ) ) )
          FC_THROW_EXCEPTION( not_contact_account, "You can only remove contact accounts!", ("account_name",account_name) );

      my->_wallet_db.remove_contact_account( account_name );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name", account_name) ) }

   void wallet::rename_account( const string& old_account_name, 
                                 const string& new_account_name )
   { try {
      if( !is_valid_account_name( old_account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid old account name!", ("old_account_name",old_account_name) );
      if( !is_valid_account_name( new_account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid new account name!", ("new_account_name",new_account_name) );

      FC_ASSERT( is_open() );

      auto old_account = my->_wallet_db.lookup_account( old_account_name );
      FC_ASSERT( old_account.valid() );
      auto old_key = old_account->owner_key;

      //Check for duplicate names in wallet
      if( !is_unique_account(old_account_name) )
      {
        //Find the wallet record that is not in the blockchain; or is, but under a different name
        auto wallet_accounts = my->_wallet_db.get_accounts();
        for( const auto& wallet_account : wallet_accounts )
        {
          if( wallet_account.second.name == old_account_name )
          {
            auto record = my->_blockchain->get_account_record(wallet_account.second.owner_key);
            if( !(record.valid() && record->name == old_account_name) )
              old_key = wallet_account.second.owner_key;
          }
        }
      }

      auto new_account = my->_wallet_db.lookup_account( new_account_name );
      FC_ASSERT( !new_account.valid() );

      my->_wallet_db.rename_account( old_key, new_account_name );
   } FC_RETHROW_EXCEPTIONS( warn, "", 
                ("old_account_name",old_account_name)
                ("new_account_name",new_account_name) ) }

   /**
    *  If we already have a key record for key, then set the private key.
    *  If we do not have a key record, 
    *     If account_name is a valid existing account, then create key record
    *       with that account as parent.
    *     If account_name is not set, then lookup account with key in the blockchain
    *       add contact account using data from blockchain and then set the private key
    */
   public_key_type wallet::import_private_key( const private_key_type& key, 
                                               const string& account_name,
                                               bool create_account )
   { try {

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto import_public_key = key.get_public_key();

      owallet_key_record current_key_record = my->_wallet_db.lookup_key( import_public_key );
      if( current_key_record.valid() )
      {
         current_key_record->encrypt_private_key( my->_wallet_password, key );
         my->_wallet_db.store_key( *current_key_record );
         return import_public_key;
      }

      auto registered_account = my->_blockchain->get_account_record( import_public_key );
      if( registered_account )
      {
          if( account_name.size() )
             FC_ASSERT( account_name == registered_account->name,
                        "Attempt to import a private key belonging to another account",
                        ("account_with_key", registered_account->name)
                        ("account_name",account_name) );

         add_contact_account( registered_account->name, import_public_key );
         return import_private_key( key, registered_account->name );
      }
      FC_ASSERT( account_name.size(), "You must specify an account name because the private key "
                                      "does not belong to any known accounts");

      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      auto account_with_key = my->_wallet_db.lookup_account( key.get_public_key() );
      if (account_with_key)
      {
          FC_ASSERT( account_name == account_with_key->name,
                     "Attempt to import a private key belonging to another account",
                     ("account_with_key", account_with_key->name)
                     ("account_name",account_name) );
      }

      auto current_account = my->_wallet_db.lookup_account( account_name );
      if( !current_account && create_account )
      {
         add_contact_account( account_name, key.get_public_key() );
         return import_private_key( key, account_name, false );
      }

      FC_ASSERT( current_account.valid(),
                "You must create an account before importing a key" );

      auto pub_key = key.get_public_key();
      address key_address(pub_key);
      current_key_record = my->_wallet_db.lookup_key( key_address );
      if( current_key_record.valid() )
      {
         FC_ASSERT( current_key_record->account_address == current_account->account_address );
         current_key_record->encrypt_private_key( my->_wallet_password, key );
         my->_wallet_db.store_key( *current_key_record );
         return current_key_record->public_key;
      }

      key_data new_key_data;
      new_key_data.account_address = current_account->account_address;
      new_key_data.encrypt_private_key( my->_wallet_password, key );

      my->_wallet_db.store_key( new_key_data );

      return pub_key;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   public_key_type wallet::import_wif_private_key( const string& wif_key, 
                                                   const string& account_name,
                                                   bool create_account )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto key = bts::utilities::wif_to_key( wif_key );
      if( key.valid() )
      {
         import_private_key( *key, account_name, create_account );
         return key->get_public_key();
      }
      
      FC_ASSERT( false, "Error parsing WIF private key" );

   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   void wallet::scan_chain( uint32_t start, uint32_t end,
                            const scan_progress_callback& progress_callback )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      elog( "WALLET SCANNING CHAIN!" );


      const auto now = blockchain::now();

      if( start == 0 )
      {
         scan_state( now );
         ++start;
      }

      if( !my->_delegate_scanning_enabled && !get_my_delegates( enabled_delegate_status ).empty() )
      {
         ulog( "\nWallet blockchain scanning disabled because there are enabled delegates!\n" );
         return;
      }

      // cancel the current scan...
      if( my->_scan_in_progress.valid() )
      {
         my->_scan_in_progress.cancel();
         try {
            my->_scan_in_progress.wait();
         } catch ( ... ) {}
      }

      my->_scan_in_progress = fc::async( [=](){ 
         auto min_end = std::min<size_t>( my->_blockchain->get_head_block_num(), end );

         try
         {
           my->_scan_progress = 0;
           auto account_priv_keys = my->_wallet_db.get_account_private_keys( my->_wallet_password );

           for( auto block_num = start; !my->_scan_in_progress.canceled() && block_num <= min_end; ++block_num )
           {
              my->scan_block( block_num, account_priv_keys, now );
              if( progress_callback )
                 progress_callback( block_num, min_end );
              my->_scan_progress = float(block_num-start)/(min_end-start+1);
              my->_wallet_db.set_property( last_unlocked_scanned_block_number, fc::variant(block_num) );
           }

           for( auto acct : my->_wallet_db.get_accounts() )
           {
              auto blockchain_acct_rec = my->_blockchain->get_account_record( acct.second.id );
              if (blockchain_acct_rec.valid())
              {
                  blockchain::account_record& brec = acct.second;
                  brec = *blockchain_acct_rec;
                  my->_wallet_db.cache_account( acct.second );
              }
           }
           my->_scan_progress = 1;
         }
         catch(...)
         {
           my->_scan_progress = -1;
           throw;
         }
      } );

   } FC_RETHROW_EXCEPTIONS( warn, "", ("start",start)("end",end) ) }

   void wallet::scan_transaction( uint32_t block_num, const transaction_id_type& transaction_id )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      const auto block = my->_blockchain->get_block( block_num );

      const auto transaction = std::find_if( block.user_transactions.begin(), block.user_transactions.end(),
                                             [transaction_id]( const signed_transaction& transaction )
                                             { return transaction.id() == transaction_id; } );
      if( transaction == block.user_transactions.end() )
          FC_THROW_EXCEPTION( transaction_not_found, "Transaction not found!",
                              ("block_num",block_num)("transaction_id",transaction_id) );

      const auto keys = my->_wallet_db.get_account_private_keys( my->_wallet_password );
      const auto now = blockchain::now();
      my->scan_transaction( *transaction, block_num, block.timestamp, keys, now );
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::scan_transactions( uint32_t block_num, const string& transaction_id_prefix )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      if( transaction_id_prefix.size() > string( transaction_id_type() ).size() )
          FC_THROW_EXCEPTION( invalid_transaction_id, "Invalid transaction id!", ("transaction_id_prefix",transaction_id_prefix) );

      const auto block = my->_blockchain->get_block( block_num );
      const auto keys = my->_wallet_db.get_account_private_keys( my->_wallet_password );
      const auto now = blockchain::now();
      bool found = false;

      for( const auto& transaction : block.user_transactions )
      {
          const auto transaction_id = string( transaction.id() );
          if( string( transaction.id() ).find( transaction_id_prefix ) != 0 ) continue;
          my->scan_transaction( transaction, block_num, block.timestamp, keys, now );
          found = true;
      }

      if( !found )
          FC_THROW_EXCEPTION( transaction_not_found, "Transaction not found!",
                              ("block_num",block_num)("transaction_id_prefix",transaction_id_prefix) );
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::sign_transaction( signed_transaction& trx, const std::unordered_set<address>& req_sigs )
   { try {
      trx.expiration = bts::blockchain::now() + BTS_BLOCKCHAIN_DEFAULT_TRANSACTION_EXPIRATION_SEC;
      for( const auto& addr : req_sigs )
      {
         trx.sign( get_private_key( addr ), my->_blockchain->chain_id()  );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("trx",trx)("req_sigs",req_sigs) ) }

   void wallet::sign_and_cache_transaction(
        signed_transaction& transaction,
        const std::unordered_set<address>& required_signatures,
        wallet_transaction_record& record )
   { try {
        sign_transaction( transaction, required_signatures );
        my->_blockchain->store_pending_transaction( transaction, true );

        const auto now = blockchain::now();
        record.record_id = transaction.id();
        record.is_virtual = false;
        record.is_confirmed = false;
        record.trx = transaction;
        record.created_time = record.received_time = now;
        my->_wallet_db.store_transaction( record );
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   slate_id_type wallet::select_slate( signed_transaction& transaction, const asset_id_type& deposit_asset_id )
   {
       auto slate_id = slate_id_type( 0 );
       if( deposit_asset_id != asset_id_type( 0 ) ) return slate_id;

       const auto slate = select_delegate_vote();
       slate_id = slate.id();

       if( slate_id != slate_id_type( 0 ) && !my->_blockchain->get_delegate_slate( slate_id ).valid() )
           transaction.define_delegate_slate( slate );

       return slate_id;
   }

   private_key_type wallet::get_private_key( const address& addr )const
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto key = my->_wallet_db.lookup_key( addr );
      FC_ASSERT( key.valid() );
      FC_ASSERT( key->has_private_key() );
      return key->decrypt_private_key( my->_wallet_password );
     } FC_RETHROW_EXCEPTIONS( warn, "", ("addr",addr) ) }

   std::string wallet::login_start(const std::string& account_name)
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto key = my->_wallet_db.lookup_key( get_account(account_name).active_address() );
      FC_ASSERT( key.valid() );
      FC_ASSERT( key->has_private_key() );

      fc::ecc::private_key one_time_key = fc::ecc::private_key::generate();
      public_key_type one_time_public_key = one_time_key.get_public_key();
      my->_login_map[one_time_public_key] = {one_time_key, fc::time_point::now()};

      std::function<void()> cleaner = [this, &cleaner]{
          std::vector<public_key_type> expired_records;
          for( const auto& record : my->_login_map )
            if( fc::time_point::now() - record.second.insertion_time >= fc::seconds(my->_login_lifetime_seconds) )
              expired_records.push_back(record.first);
          ilog("Purging ${count} expired records from login map.", ("count", expired_records.size()));
          for( const auto& record : expired_records )
            my->_login_map.erase(record);

          if( !my->_login_map.empty() )
            my->_login_map_cleaner = fc::thread::current().schedule(cleaner,
                                                                    fc::time_point::now()
                                                                    + fc::seconds(my->_login_cleaner_interval_seconds));
          else
            my->_login_map_cleaner = fc::future<void>();
      };
      if( !my->_login_map_cleaner.valid() )
        my->_login_map_cleaner = fc::thread::current().schedule(cleaner,
                                                                fc::time_point::now()
                                                                + fc::seconds(my->_login_cleaner_interval_seconds));

      auto signature = key->decrypt_private_key(my->_wallet_password)
                          .sign_compact(fc::sha256::hash((char*)&one_time_public_key,
                                                         sizeof(one_time_public_key)));

      return CUSTOM_URL_SCHEME ":Login/" + variant(public_key_type(one_time_public_key)).as_string()
                                         + "/" + fc::variant(signature).as_string() + "/";
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   fc::variant wallet::login_finish(const public_key_type& server_key,
                                    const public_key_type& client_key,
                                    const fc::ecc::compact_signature& client_signature)
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( my->_login_map.find(server_key) != my->_login_map.end(), "Login session has expired. Generate a new login URL and try again." );

      fc::ecc::private_key private_key = my->_login_map[server_key].key;
      my->_login_map.erase(server_key);
      auto secret = private_key.get_shared_secret( fc::ecc::public_key_data(client_key) );
      auto user_account_key = fc::ecc::public_key(client_signature, fc::sha256::hash(secret.data(), sizeof(secret)));

      fc::mutable_variant_object result;
      result["user_account_key"] = public_key_type(user_account_key);
      result["shared_secret"] = secret;
      return result;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("server_key",server_key)("client_key",client_key)("client_signature",client_signature) ) }

   /** 
    * @return the list of all transactions related to this wallet
    */
   vector<wallet_transaction_record> wallet::get_transaction_history( const string& account_name,
                                                                      uint32_t start_block_num,
                                                                      uint32_t end_block_num,
                                                                      const string& asset_symbol )const
   { try {
      FC_ASSERT( is_open() );
      if( end_block_num != -1 ) FC_ASSERT( start_block_num <= end_block_num );

      vector<wallet_transaction_record> history_records;
      const auto& transactions = my->_wallet_db.get_transactions();

      auto asset_id = 0;
      if( !asset_symbol.empty() && asset_symbol != BTS_BLOCKCHAIN_SYMBOL )
      {
          try
          {
              asset_id = my->_blockchain->get_asset_id( asset_symbol );
          }
          catch( const fc::exception& )
          {
              FC_THROW_EXCEPTION( invalid_asset_symbol, "Invalid asset symbol!", ("asset_symbol",asset_symbol) );
          }
      }

      for( const auto& item : transactions )
      {
          const auto& tx_record = item.second;

          if( tx_record.block_num < start_block_num ) continue;
          if( end_block_num != -1 && tx_record.block_num > end_block_num ) continue;
          if( tx_record.ledger_entries.empty() ) continue; /* TODO: Temporary */

          if( !account_name.empty() )
          {
              bool match = false;
              if( tx_record.ledger_entries.front().from_account.valid() ) match |= get_key_label( *tx_record.ledger_entries.front().from_account ) == account_name;
              if( tx_record.ledger_entries.front().to_account.valid() ) match |= get_key_label( *tx_record.ledger_entries.front().to_account ) == account_name;
              if( !match ) continue;
          }

          if( asset_id != 0 )
          {
              bool match = false;
              match |= tx_record.ledger_entries.front().amount.asset_id == asset_id;
              match |= tx_record.ledger_entries.front().memo.find( asset_symbol ) != string::npos; /* TODO: This is dumb */
              if( !match ) continue;
          }

          history_records.push_back( tx_record );
      }

      return history_records;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   vector<pretty_transaction> wallet::get_pretty_transaction_history( const string& account_name,
                                                                      uint32_t start_block_num,
                                                                      uint32_t end_block_num,
                                                                      const string& asset_symbol )const
   { try {
       const auto& history = get_transaction_history( account_name, start_block_num, end_block_num, asset_symbol );
       vector<pretty_transaction> pretties;
       pretties.reserve( history.size() );
       for( const auto& item : history ) pretties.push_back( to_pretty_trx( item ) );

       std::sort( pretties.begin(), pretties.end(),
                  []( const pretty_transaction& a, const pretty_transaction& b ) -> bool
                  {
                     if( a.received_time != b.received_time) return a.received_time < b.received_time;
                     if( a.block_num != b.block_num ) return a.block_num < b.block_num;
                     return string( a.trx_id ).compare( string( b.trx_id ) ) < 0;
                  } );

       // TODO: Handle pagination

       /* Don't care if not filtering by account */
       if( account_name.empty() ) return pretties;

       /* Tally up running balances */
       auto running_balances = map<asset_id_type, asset>();
       for( auto& trx : pretties )
       {
           const auto fee_asset_id = trx.fee.asset_id;
           if( trx.running_balances.count( fee_asset_id ) <= 0 )
                trx.running_balances[ fee_asset_id ] = asset( 0, fee_asset_id );

           auto any_from_me = false;

           for( auto& entry : trx.ledger_entries )
           {
               const auto amount_asset_id = entry.amount.asset_id;
               if( running_balances.count( amount_asset_id ) <= 0 )
                   running_balances[ amount_asset_id ] = asset( 0, amount_asset_id );

               auto from_me = false;
               from_me |= account_name.empty() && is_receive_account( entry.from_account );
               from_me |= account_name == entry.from_account;
               if( from_me ) running_balances[ amount_asset_id ] -= entry.amount;

               auto to_me = false;
               to_me |= account_name.empty() && is_receive_account( entry.to_account );
               to_me |= account_name == entry.to_account;
               if( to_me ) running_balances[ amount_asset_id ] += entry.amount;

               trx.running_balances[ amount_asset_id ] = running_balances[ amount_asset_id ];

               any_from_me |= from_me;
           }

           /* Subtract fee from running balances */
           if( !trx.is_virtual && (any_from_me || trx.is_market_cancel) ) trx.running_balances[ fee_asset_id ] -= trx.fee;
           else trx.fee = asset(); /* Don't return fees we didn't pay */
       }

       return pretties;
   } FC_RETHROW_EXCEPTIONS( warn, "" ) }

   void wallet::set_delegate_block_production( const string& delegate_name, bool enabled )
   {
      FC_ASSERT( is_open() );
      std::vector<wallet_account_record> delegate_records;

      if( delegate_name != "ALL" )
      {
          if( !is_valid_account_name( delegate_name ) )
              FC_THROW_EXCEPTION( invalid_name, "Invalid delegate name!", ("delegate_name",delegate_name) );

          auto delegate_record = get_account( delegate_name );
          FC_ASSERT( delegate_record.is_delegate(), "${name} is not a delegate.", ("name", delegate_name) );
          auto key = my->_wallet_db.lookup_key( delegate_record.active_key() );
          FC_ASSERT( key.valid() && key->has_private_key(), "Unable to find private key for ${name}.", ("name", delegate_name) );
          delegate_records.push_back( delegate_record );
      }
      else
      {
          delegate_records = get_my_delegates( any_delegate_status );
      }

      for( auto& delegate_record : delegate_records )
      {
          delegate_record.block_production_enabled = enabled;
          my->_wallet_db.cache_account( delegate_record ); //store_record( *delegate_record );
      }
   }

   void wallet::set_delegate_transaction_scanning( bool enabled )
   {
      FC_ASSERT( is_open() );
      my->_delegate_scanning_enabled = enabled;
   }

   vector<wallet_account_record> wallet::get_my_delegates(int delegates_to_retrieve)const
   {
      FC_ASSERT( is_open() );
      vector<wallet_account_record> delegate_records;
      const auto& account_records = list_my_accounts();
      for( const auto& account_record : account_records )
      {
          if( !account_record.is_delegate() ) continue;
          if( delegates_to_retrieve & enabled_delegate_status && !account_record.block_production_enabled ) continue;
          if( delegates_to_retrieve & disabled_delegate_status && account_record.block_production_enabled ) continue;
          if( delegates_to_retrieve & active_delegate_status && !my->_blockchain->is_active_delegate( account_record.id ) ) continue;
          if( delegates_to_retrieve & inactive_delegate_status && my->_blockchain->is_active_delegate( account_record.id ) ) continue;
          delegate_records.push_back( account_record );
      }
      return delegate_records;
   }

   vector<private_key_type> wallet::get_my_delegate_private_keys(int delegates_to_retrieve )const
   {
       vector<private_key_type> private_keys;
       const auto& delegate_records = get_my_delegates( delegates_to_retrieve );
       for( const auto& delegate_record : delegate_records )
          private_keys.push_back( get_private_key( address( delegate_record.active_key() ) ) );
       return private_keys;
   }

   optional<time_point_sec> wallet::get_next_producible_block_timestamp( const vector<wallet_account_record>& delegate_records )const
   { try {
      if( !is_open() || is_locked() ) return optional<time_point_sec>();

      vector<account_id_type> delegate_ids;
      delegate_ids.reserve( delegate_records.size() );
      for( const auto& delegate_record : delegate_records )
          delegate_ids.push_back( delegate_record.id );

      return my->_blockchain->get_next_producible_block_timestamp( delegate_ids );
   } FC_CAPTURE_AND_RETHROW() }

   void wallet::sign_block( signed_block_header& header )const
   { try {
      FC_ASSERT( is_unlocked() );
      if( header.timestamp == fc::time_point_sec() )
          FC_THROW_EXCEPTION( invalid_timestamp, "Invalid block timestamp! Block production may be disabled" );

      auto delegate_record = my->_blockchain->get_slot_signee( header.timestamp, my->_blockchain->get_active_delegates() );
      auto delegate_pub_key = delegate_record.active_key();
      auto delegate_key = get_private_key( address(delegate_pub_key) );
      FC_ASSERT( delegate_pub_key == delegate_key.get_public_key() );

      header.previous_secret = my->get_secret( delegate_record.delegate_info->last_block_num_produced,
                                               delegate_key );
      auto next_secret = my->get_secret( my->_blockchain->get_head_block_num() + 1, delegate_key );
      header.next_secret_hash = fc::ripemd160::hash( next_secret );

      header.sign( delegate_key, my->_blockchain->chain_id() );
      FC_ASSERT( header.validate_signee( delegate_pub_key, my->_blockchain->chain_id() ) );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("header",header) ) }

   /**
    *  This method assumes that fees can be paid in the same asset type as the 
    *  asset being transferred so that the account can be kept private and 
    *  secure.
    *
    */
   // TODO: This is broken
   vector<signed_transaction> wallet::multipart_transfer( double  real_amount_to_transfer,
                                                const string& amount_to_transfer_symbol,
                                                const string& from_account_name,
                                                const string& to_account_name,
                                                const string& memo_message,
                                                bool          sign )
   { try {
       FC_ASSERT( is_open() );
       FC_ASSERT( is_unlocked() );

       if( !my->_blockchain->is_valid_symbol( amount_to_transfer_symbol ) )
           FC_THROW_EXCEPTION( invalid_asset_symbol, "Invalid asset symbol!", ("amount_to_transfer_symbol",amount_to_transfer_symbol) );

       if( !is_receive_account( from_account_name ) )
           FC_THROW_EXCEPTION( unknown_account, "Unknown sending account name!", ("from_account_name",from_account_name) );

       if( !is_valid_account( to_account_name ) )
           FC_THROW_EXCEPTION( unknown_receive_account, "Unknown receiving account name!", ("to_account_name",to_account_name) );

       if( !is_unique_account(to_account_name) )
           FC_THROW_EXCEPTION( duplicate_account_name,
                               "Local account name conflicts with registered name. Please rename your local account first.", ("to_account_name",to_account_name) );

       if( memo_message.size() > BTS_BLOCKCHAIN_MAX_MEMO_SIZE )
           FC_THROW_EXCEPTION( memo_too_long, "Memo too long!", ("memo_message",memo_message) );

       auto asset_rec = my->_blockchain->get_asset_record( amount_to_transfer_symbol );
       FC_ASSERT( asset_rec.valid() );
       auto asset_id = asset_rec->id;

       int64_t precision = asset_rec->precision ? asset_rec->precision : 1;
       share_type amount_to_transfer((share_type)(real_amount_to_transfer * precision));
       asset asset_to_transfer( amount_to_transfer, asset_id );

       //FC_ASSERT( amount_to_transfer > get_priority_fee( amount_to_transfer_symbol ).amount );

       /**
        *  TODO: until we support paying fees in other assets, this will not function
        *  properly.
        */
       FC_ASSERT( asset_id == 0, "multipart transfers only support base shares",
                  ("asset_to_transfer",asset_to_transfer)("symbol",amount_to_transfer_symbol));

       vector<signed_transaction >       trxs;
       vector<share_type>                amount_sent;
       vector<wallet_balance_record>     balances_to_store; // records to cache if transfer succeeds

       public_key_type  receiver_public_key = get_account_public_key( to_account_name );
       private_key_type sender_private_key  = get_account_private_key( from_account_name );
       public_key_type  sender_public_key   = sender_private_key.get_public_key();
       address          sender_account_address( sender_private_key.get_public_key() );
       
       asset total_fee = get_priority_fee();

       asset amount_collected( 0, asset_id );
       for( auto balance_item : my->_wallet_db.get_balances() )
       {
          auto owner = balance_item.second.owner();
          if( balance_item.second.asset_id() == asset_id &&
              my->address_in_account( owner, sender_account_address ) )
          {
             signed_transaction trx;

             auto from_balance = balance_item.second.get_balance();

             if( from_balance.amount <= 0 ) 
                continue;

             trx.withdraw( balance_item.first,
                           from_balance.amount );

             from_balance -= total_fee;

             /** make sure there is at least something to withdraw at the other side */
             if( from_balance < total_fee )
                continue; // next balance_item

             asset amount_to_deposit( 0, asset_id );
             asset amount_of_change( 0, asset_id );

             if( (amount_collected + from_balance) > asset_to_transfer )
             {
                amount_to_deposit = asset_to_transfer - amount_collected;
                amount_of_change  = from_balance - amount_to_deposit;
                amount_collected += amount_to_deposit;
             }
             else
             {
                amount_to_deposit = from_balance;
             }

             const auto slate_id = select_slate( trx, amount_to_deposit.asset_id );

             if( amount_to_deposit.amount > 0 )
             {
                trx.deposit_to_account( receiver_public_key,
                                        amount_to_deposit,
                                        sender_private_key,
                                        memo_message,
                                        slate_id,
                                        sender_private_key.get_public_key(),
                                        my->create_one_time_key(),
                                        from_memo
                                        );
             }
             if( amount_of_change > total_fee )
             {
                trx.deposit_to_account( sender_public_key,
                                        amount_of_change,
                                        sender_private_key,
                                        memo_message,
                                        slate_id,
                                        receiver_public_key,
                                        my->create_one_time_key(),
                                        to_memo
                                        );

                /** randomly shuffle change to prevent analysis */
                if( rand() % 2 ) 
                {
                   FC_ASSERT( trx.operations.size() >= 3 )
                   std::swap( trx.operations[1], trx.operations[2] );
                }
             }

             // set the balance of this item to 0 so that we do not
             // attempt to spend it again.
             balance_item.second.balance = 0;
             balances_to_store.push_back( balance_item.second );

             if( sign )
             {
                unordered_set<address> required_signatures;
                required_signatures.insert( balance_item.second.owner() );
                sign_transaction( trx, required_signatures );
             }

             trxs.emplace_back( trx );
             amount_sent.push_back( amount_to_deposit.amount );
             if( amount_collected >= asset( amount_to_transfer, asset_id ) )
                break;
          } // if asset id matches
       } // for each balance_item

       // If we went through all our balances and still don't have enough
       if (amount_collected < asset( amount_to_transfer, asset_id ))
       {
          FC_ASSERT( !"Insufficient funds.");
       }

       if( sign ) // don't store invalid trxs..
       {
          //const auto now = blockchain::now();
          for( const auto& rec : balances_to_store )
          {
              my->_wallet_db.cache_balance( rec );
          }
          for( uint32_t i =0 ; i < trxs.size(); ++i )
          {
              // TODO: FIXME
              /*
             my->_wallet_db.cache_transaction( trxs[i], asset( -amount_sent[i], asset_id), 
                                               total_fee.amount, 
                                               memo_message, 
                                               receiver_public_key,
                                               now,
                                               now,
                                               sender_public_key
                                             );
                                             */
          }
       }

       return trxs;
      
   } FC_RETHROW_EXCEPTIONS( warn, "", 
         ("amount_to_transfer",real_amount_to_transfer)
         ("amount_to_transfer_symbol",amount_to_transfer_symbol)
         ("from_account_name",from_account_name)
         ("to_account_name",to_account_name)
         ("memo_message",memo_message) ) }

   signed_transaction wallet::withdraw_delegate_pay( const string& delegate_name,
                                                     double real_amount_to_withdraw,
                                                     const string& withdraw_to_account_name,
                                                     const string& memo_message,
                                                     bool sign )
   { try {
       FC_ASSERT( is_open() );
       FC_ASSERT( is_unlocked() );
       FC_ASSERT( is_receive_account( delegate_name ) );
       FC_ASSERT( is_valid_account( withdraw_to_account_name ) );

       auto asset_rec = my->_blockchain->get_asset_record( asset_id_type(0) );
       share_type amount_to_withdraw((share_type)(real_amount_to_withdraw * asset_rec->get_precision()));

       auto delegate_account_record = my->_blockchain->get_account_record( delegate_name ); //_wallet_db.lookup_account( delegate_name );
       FC_ASSERT( delegate_account_record.valid() );
       FC_ASSERT( delegate_account_record->is_delegate() );

       auto required_fees = get_priority_fee();
       FC_ASSERT( delegate_account_record->delegate_info->pay_balance >= (amount_to_withdraw + required_fees.amount), "",
                  ("delegate_account_record",delegate_account_record));

       signed_transaction trx;
       unordered_set<address> required_signatures;

       owallet_key_record delegate_key = my->_wallet_db.lookup_key( delegate_account_record->active_key() );
       FC_ASSERT( delegate_key && delegate_key->has_private_key() );
       auto delegate_private_key = delegate_key->decrypt_private_key( my->_wallet_password );
       required_signatures.insert( delegate_private_key.get_public_key() );

       public_key_type  receiver_public_key = get_account_public_key( withdraw_to_account_name );

       const auto slate_id = select_slate( trx );
       
       trx.withdraw_pay( delegate_account_record->id, amount_to_withdraw + required_fees.amount );
       trx.deposit_to_account( receiver_public_key,
                               asset(amount_to_withdraw,0),
                               delegate_private_key,
                               memo_message,
                               slate_id,
                               delegate_private_key.get_public_key(),
                               my->create_one_time_key(),
                               from_memo
                               );

       if( sign )
       {
          auto entry = ledger_entry();
          entry.from_account = delegate_private_key.get_public_key();
          entry.to_account = receiver_public_key;
          entry.amount = asset( amount_to_withdraw );
          entry.memo = memo_message;

          auto record = wallet_transaction_record();
          record.ledger_entries.push_back( entry );
          record.fee = required_fees;

          sign_and_cache_transaction( trx, required_signatures, record );
       }
       return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name",delegate_name)
                                      ("amount_to_withdraw",real_amount_to_withdraw ) ) }

   signed_transaction  wallet::transfer_asset_to_address( double real_amount_to_transfer,
                                                          const string& amount_to_transfer_symbol,
                                                          const string& from_account_name,
                                                          const address& to_address,
                                                          const string& memo_message,
                                                          bool sign )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( my->_blockchain->is_valid_symbol( amount_to_transfer_symbol ) );
      FC_ASSERT( is_receive_account( from_account_name ) );
      
      const auto asset_rec = my->_blockchain->get_asset_record( amount_to_transfer_symbol );
      FC_ASSERT( asset_rec.valid() );
      const auto asset_id = asset_rec->id;
      
      const int64_t precision = asset_rec->precision ? asset_rec->precision : 1;
      share_type amount_to_transfer = real_amount_to_transfer * precision;
      asset asset_to_transfer( amount_to_transfer, asset_id );

      private_key_type sender_private_key  = get_account_private_key( from_account_name );
      public_key_type  sender_public_key   = sender_private_key.get_public_key();
      address          sender_account_address( sender_private_key.get_public_key() );

      signed_transaction     trx;
      unordered_set<address> required_signatures;
        
      const auto required_fees = get_priority_fee();
      if( required_fees.asset_id == asset_to_transfer.asset_id )
      {
         my->withdraw_to_transaction( required_fees.amount + amount_to_transfer,
                                      required_fees.asset_id,
                                      sender_account_address,
                                      trx, required_signatures );
      }
      else
      {
         my->withdraw_to_transaction( asset_to_transfer.amount,
                                      asset_to_transfer.asset_id,
                                      sender_account_address,
                                      trx, required_signatures );
         
         my->withdraw_to_transaction( required_fees.amount,
                                      required_fees.asset_id,
                                      sender_account_address,
                                      trx, required_signatures );
      }

      const auto slate_id = select_slate( trx, asset_to_transfer.asset_id );

      trx.deposit( to_address, asset_to_transfer, slate_id);

      if( sign )
      {
         auto entry = ledger_entry();
         entry.from_account = sender_public_key;
         entry.amount = asset_to_transfer;
         entry.memo = memo_message;

         auto record = wallet_transaction_record();
         record.ledger_entries.push_back( entry );
         record.fee = required_fees;
         record.extra_addresses.push_back( to_address );
      }
      return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "",
                            ("real_amount_to_transfer",real_amount_to_transfer)
                            ("amount_to_transfer_symbol",amount_to_transfer_symbol)
                            ("from_account_name",from_account_name)
                            ("to_address",to_address)
                            ("memo_message",memo_message) ) }
    
   signed_transaction  wallet::transfer_asset_to_many_address( const string& amount_to_transfer_symbol,
                                                       const string& from_account_name,
                                                       const std::unordered_map< address, double >& to_address_amounts,
                                                       const string& memo_message,
                                                       bool sign )
   {
      try {
         FC_ASSERT( is_open() );
         FC_ASSERT( is_unlocked() );
         FC_ASSERT( my->_blockchain->is_valid_symbol( amount_to_transfer_symbol ) );
         FC_ASSERT( is_receive_account( from_account_name ) );
         FC_ASSERT( to_address_amounts.size() > 0 );
         
         auto asset_rec = my->_blockchain->get_asset_record( amount_to_transfer_symbol );
         FC_ASSERT( asset_rec.valid() );
         auto asset_id = asset_rec->id;
         
         private_key_type sender_private_key  = get_account_private_key( from_account_name );
         public_key_type  sender_public_key   = sender_private_key.get_public_key();
         address          sender_account_address( sender_private_key.get_public_key() );
         
         signed_transaction     trx;
         unordered_set<address> required_signatures;
         
         asset total_asset_to_transfer( 0, asset_id );
         auto required_fees = get_priority_fee();
         
         vector<address> to_addresses;
         for( const auto& address_amount : to_address_amounts )
         {
            auto real_amount_to_transfer = address_amount.second;
            share_type amount_to_transfer((share_type)(real_amount_to_transfer * asset_rec->get_precision()));
            asset asset_to_transfer( amount_to_transfer, asset_id );
            
            my->withdraw_to_transaction( amount_to_transfer,
                                        asset_to_transfer.asset_id,
                                        sender_account_address,
                                        trx, required_signatures );
            
            total_asset_to_transfer += asset_to_transfer;
             
            trx.deposit( address_amount.first, asset_to_transfer, 0 );
             
            to_addresses.push_back( address_amount.first );
         }
         
         my->withdraw_to_transaction( required_fees.amount,
                                     required_fees.asset_id,
                                     sender_account_address,
                                     trx, required_signatures );
         
         if( sign )
         {
            auto entry = ledger_entry();
            entry.from_account = sender_public_key;
            entry.amount = total_asset_to_transfer;
            entry.memo = memo_message;

            auto record = wallet_transaction_record();
            record.ledger_entries.push_back( entry );
            record.fee = required_fees;
            record.extra_addresses = to_addresses;

            sign_and_cache_transaction( trx, required_signatures, record );
         }
         return trx;
      } FC_RETHROW_EXCEPTIONS( warn, "",
                              ("amount_to_transfer_symbol",amount_to_transfer_symbol)
                              ("from_account_name",from_account_name)
                              ("to_address_amounts",to_address_amounts)
                              ("memo_message",memo_message) ) }
   
   signed_transaction wallet::transfer_asset( double real_amount_to_transfer,
                                        const string& amount_to_transfer_symbol,
                                        const string& from_account_name,
                                        const string& to_account_name,
                                        const string& memo_message,
                                        bool sign )
   { try {
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      if( !my->_blockchain->is_valid_symbol( amount_to_transfer_symbol ) )
          FC_THROW_EXCEPTION( invalid_asset_symbol, "Invalid asset symbol!", ("amount_to_transfer_symbol",amount_to_transfer_symbol) );

      if( !is_receive_account( from_account_name ) )
          FC_THROW_EXCEPTION( unknown_account, "Unknown sending account name!", ("from_account_name",from_account_name) );

      if( !is_valid_account( to_account_name ) )
          FC_THROW_EXCEPTION( unknown_receive_account, "Unknown receiving account name!", ("to_account_name",to_account_name) );

      if( !is_unique_account(to_account_name) )
          FC_THROW_EXCEPTION( duplicate_account_name,
                              "Local account name conflicts with registered name. Please rename your local account first.", ("to_account_name",to_account_name) );

      if( memo_message.size() > BTS_BLOCKCHAIN_MAX_MEMO_SIZE )
          FC_THROW_EXCEPTION( memo_too_long, "Memo too long!", ("memo_message",memo_message) );

      const auto asset_rec = my->_blockchain->get_asset_record( amount_to_transfer_symbol );
      FC_ASSERT( asset_rec.valid() );
      const auto asset_id = asset_rec->id;
      
      share_type amount_to_transfer = real_amount_to_transfer * asset_rec->get_precision();
      asset asset_to_transfer( amount_to_transfer, asset_id );

      public_key_type  receiver_public_key = get_account_public_key( to_account_name );
      private_key_type sender_private_key  = get_account_private_key( from_account_name );
      public_key_type  sender_public_key   = sender_private_key.get_public_key();
      address          sender_account_address( sender_private_key.get_public_key() );
        
      signed_transaction     trx;
      unordered_set<address> required_signatures;
        
      const auto required_fees = get_priority_fee();
      if( required_fees.asset_id == asset_to_transfer.asset_id )
      {
         my->withdraw_to_transaction( required_fees.amount + amount_to_transfer,
                                       required_fees.asset_id,
                                       sender_account_address,
                                       trx, required_signatures );
      }
      else
      {
         my->withdraw_to_transaction( asset_to_transfer.amount,
                                       asset_to_transfer.asset_id,
                                       sender_account_address,
                                       trx, required_signatures );
         
         my->withdraw_to_transaction( required_fees.amount,
                                       required_fees.asset_id,
                                       sender_account_address,
                                       trx, required_signatures );
      }

      const auto slate_id = select_slate( trx, asset_to_transfer.asset_id );

      trx.deposit_to_account( receiver_public_key,
                              asset_to_transfer,
                              sender_private_key,
                              memo_message,
                              slate_id,
                              sender_private_key.get_public_key(),
                              my->create_one_time_key(),
                              from_memo
                              );
        
      if( sign )
      {
         auto entry = ledger_entry();
         entry.from_account = sender_public_key;
         entry.to_account = receiver_public_key;
         entry.amount = asset_to_transfer;
         entry.memo = memo_message;

         auto record = wallet_transaction_record();
         record.ledger_entries.push_back( entry );
         record.fee = required_fees;

         sign_and_cache_transaction( trx, required_signatures, record );
      }
      return trx;
   } FC_CAPTURE_AND_RETHROW( (real_amount_to_transfer)
                             (amount_to_transfer_symbol)
                             (from_account_name)
                             (to_account_name)
                             (memo_message ) ) }

   signed_transaction wallet::register_account( const string& account_to_register,
                                                const variant& public_data,
                                                uint8_t delegate_pay_rate,
                                                const string& pay_with_account_name,
                                                bool sign )
   { try {
      if( !is_valid_account_name( account_to_register ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_to_register",account_to_register) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto registered_account = my->_blockchain->get_account_record( account_to_register );
      FC_ASSERT( !registered_account.valid(), "the account has already been registered",
                 ("account_to_register",account_to_register) );

      auto payer_public_key = get_account_public_key( pay_with_account_name );
      address from_account_address( payer_public_key );

      auto account_public_key = get_account_public_key( account_to_register );

      signed_transaction     trx;
      unordered_set<address> required_signatures;

      trx.register_account( account_to_register,
                            public_data,
                            account_public_key, // master
                            account_public_key, // active
                            delegate_pay_rate <= 100 ? delegate_pay_rate : 255 );

      auto pos = account_to_register.find( '.' );
      if( pos != string::npos )
      {
          auto parent_name = account_to_register.substr( pos+1, string::npos );
          auto parent_acct = get_account( parent_name );
          required_signatures.insert( parent_acct.active_address() );
      }

      auto required_fees = get_priority_fee();

      bool as_delegate = false;
      if( delegate_pay_rate <= 100  )
      {
        required_fees += asset((delegate_pay_rate * my->_blockchain->get_delegate_registration_fee())/100,0);
        as_delegate = true;
      }

      auto size_fee = fc::raw::pack_size( public_data );
      required_fees += asset( my->_blockchain->calculate_data_fee(size_fee) );

      // TODO: adjust fee based upon blockchain price per byte and
      // the size of trx... 'recursively'

      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   from_account_address,
                                   trx, required_signatures );

      if( sign )
      {
         auto entry = ledger_entry();
         entry.from_account = payer_public_key;
         entry.to_account = account_public_key;
         entry.memo = "register " + account_to_register + (as_delegate ? " as a delegate" : "");

         auto record = wallet_transaction_record();
         record.ledger_entries.push_back( entry );
         record.fee = required_fees;

         sign_and_cache_transaction( trx, required_signatures, record );
      }
      return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_to_register",account_to_register)
                                      ("public_data", public_data)
                                      ("pay_with_account_name", pay_with_account_name) 
                                      ("delegate_pay_rate",int(delegate_pay_rate)) ) }

   signed_transaction  wallet::create_asset( const string& symbol,
                                             const string& asset_name,
                                             const string& description,
                                             const variant& data,
                                             const string& issuer_account_name,
                                             double max_share_supply, 
                                             int64_t precision,
                                             bool is_market_issued,
                                             bool sign  )
   { try {

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( my->_blockchain->is_valid_symbol_name( symbol ) ); // valid length and characters
      FC_ASSERT( ! my->_blockchain->is_valid_symbol( symbol ) ); // not yet registered

      signed_transaction     trx;
      unordered_set<address> required_signatures;


      // TODO: adjust fee based upon blockchain price per byte and
      // the size of trx... 'recursively'
      auto required_fees = get_priority_fee();

      auto size_fee = fc::raw::pack_size( data );
      required_fees += asset( my->_blockchain->calculate_data_fee(size_fee) );
      required_fees += asset(my->_blockchain->get_asset_registration_fee(),0);

      if( !is_valid_account_name( issuer_account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("issuer_account_name",issuer_account_name) );
      auto from_account_address = get_account_public_key( issuer_account_name );
      auto oname_rec = my->_blockchain->get_account_record( issuer_account_name );
      FC_ASSERT( oname_rec.valid() );


      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   from_account_address,
                                   trx, required_signatures );
    
      //check this way to avoid overflow
      FC_ASSERT(BTS_BLOCKCHAIN_MAX_SHARES / precision > max_share_supply);      
      share_type max_share_supply_in_internal_units = max_share_supply * precision;
      if( NOT is_market_issued )
      {
         required_signatures.insert( address( from_account_address ) );
         trx.create_asset( symbol, asset_name,
                           description, data,
                           oname_rec->id, max_share_supply, precision );
      }
      else
      {
         trx.create_asset( symbol, asset_name,
                           description, data,
                           asset_record::market_issued_asset, max_share_supply_in_internal_units, precision );
      }

      if( sign )
      {
         auto entry = ledger_entry();
         entry.from_account = from_account_address;
         entry.to_account = from_account_address;
         entry.memo = "create " + symbol + " (" + asset_name + ")";

         auto record = wallet_transaction_record();
         record.ledger_entries.push_back( entry );
         record.fee = required_fees;

         sign_and_cache_transaction( trx, required_signatures, record );
      }
      return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("symbol",symbol)
                                      ("name", asset_name )
                                      ("description", description)
                                      ( "issuer_account", issuer_account_name) ) }

   signed_transaction wallet::issue_asset( double amount_to_issue, 
                                           const string& symbol,                                               
                                           const string& to_account_name,
                                           const string& memo_message,
                                           bool sign )
   {
      if( !is_valid_account_name( to_account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("to_account_name",to_account_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      FC_ASSERT( my->_blockchain->is_valid_symbol( symbol ) );

      signed_transaction         trx;
      unordered_set<address>     required_signatures;
      
      auto required_fees = get_priority_fee();

      auto asset_record = my->_blockchain->get_asset_record( symbol );
      FC_ASSERT(asset_record.valid(), "no such asset record");
      auto issuer_account = my->_blockchain->get_account_record( asset_record->issuer_account_id );
      FC_ASSERT(issuer_account, "uh oh! no account for valid asset");

      asset shares_to_issue( amount_to_issue * asset_record->get_precision(), asset_record->id );
      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   get_account_public_key( issuer_account->name ),
                                   trx, required_signatures );
     
      trx.issue( shares_to_issue );
      required_signatures.insert( issuer_account->active_key() );

      public_key_type receiver_public_key = get_account_public_key( to_account_name );
      owallet_account_record issuer = my->_wallet_db.lookup_account( asset_record->issuer_account_id );
      FC_ASSERT( issuer.valid() );
      owallet_key_record  issuer_key = my->_wallet_db.lookup_key( issuer->account_address );
      FC_ASSERT( issuer_key && issuer_key->has_private_key() );
      auto sender_private_key = issuer_key->decrypt_private_key( my->_wallet_password );

      trx.deposit_to_account( receiver_public_key,
                              shares_to_issue,
                              sender_private_key,
                              memo_message,
                              0,
                              sender_private_key.get_public_key(),
                              my->create_one_time_key(),
                              from_memo
                              );

      if( sign )
      {
          auto entry = ledger_entry();
          entry.from_account = issuer->active_key();
          entry.to_account = receiver_public_key;
          entry.amount = shares_to_issue;
          entry.memo = "issue " + my->_blockchain->to_pretty_asset( shares_to_issue );

          auto record = wallet_transaction_record();
          record.ledger_entries.push_back( entry );
          record.fee = required_fees;

          sign_and_cache_transaction( trx, required_signatures, record );
      }
      return trx;
   }

   void wallet::update_account_private_data( const string& account_to_update,
                                             const variant& private_data )
   {
      get_account( account_to_update ); /* Just to check input */
      auto oacct = my->_wallet_db.lookup_account( account_to_update );

      oacct->private_data = private_data;
      my->_wallet_db.cache_account( *oacct );
   }

   signed_transaction wallet::update_registered_account( const string& account_to_update,
                                                                const string& pay_from_account,
                                                                optional<variant> public_data,
                                                                uint8_t delegate_pay_rate,
                                                                bool sign )
   { try {
      if( !is_valid_account_name( account_to_update ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_to_update",account_to_update) );
      if( !is_valid_account_name( pay_from_account ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("pay_from_account",pay_from_account) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      signed_transaction trx;
      unordered_set<address>     required_signatures;
      auto payer_public_key = get_account_public_key( pay_from_account );

      auto account = my->_blockchain->get_account_record( account_to_update );
      if( !account.valid() )
        FC_THROW_EXCEPTION( unknown_account, "Unknown account!", ("account_to_update",account_to_update) );

      auto account_public_key = get_account_public_key( account_to_update );
      auto required_fees = get_priority_fee();

      if( account->is_delegate() )
      {
         if( delegate_pay_rate > account->delegate_info->pay_rate )
            FC_THROW_EXCEPTION( invalid_pay_rate, "Pay rate can only be decreased!", ("delegate_pay_rate",delegate_pay_rate) );
      }
      else
      {
         if( delegate_pay_rate <= 100  )
         {
           required_fees += asset((delegate_pay_rate * my->_blockchain->get_delegate_registration_fee())/100,0);
         }
      }

      auto size_fee = fc::raw::pack_size( public_data );
      required_fees += asset( my->_blockchain->calculate_data_fee(size_fee) );

      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   payer_public_key,
                                   trx, required_signatures );
     
      required_signatures.insert( account->active_key() ); 
    
      trx.update_account( account->id, delegate_pay_rate, public_data, optional<public_key_type>() );
       
      if (sign)
      {
          auto entry = ledger_entry();
          entry.from_account = payer_public_key;
          entry.to_account = account_public_key;
          entry.memo = "update " + account_to_update;

          auto record = wallet_transaction_record();
          record.ledger_entries.push_back( entry );
          record.fee = required_fees;

          sign_and_cache_transaction( trx, required_signatures, record );
      }
      return trx;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_to_update",account_to_update)
                                      ("pay_from_account",pay_from_account)
                                      ("public_data",public_data)
                                      ("sign",sign) ) }

   signed_transaction wallet::create_proposal( const string& delegate_account_name,
                                       const string& subject,
                                       const string& body,
                                       const string& proposal_type,
                                       const variant& data,
                                       bool sign  )
   {
      if( !is_valid_account_name( delegate_account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("delegate_account_name",delegate_account_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      // TODO validate subject, body, and data

      signed_transaction trx;
      unordered_set<address>     required_signatures;

      auto delegate_account = my->_blockchain->get_account_record( delegate_account_name );
      FC_ASSERT(delegate_account.valid(), "No such account: ${acct}", ("acct", delegate_account_name));
      
      auto required_fees = get_priority_fee();

      trx.submit_proposal( delegate_account->id, subject, body, proposal_type, data );
      required_fees += asset( my->_blockchain->calculate_data_fee( fc::raw::pack_size(trx) ), 0 );

      /*
      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   get_account_public_key( delegate_account->name ),
                                   trx, required_signatures );
      */
     
      trx.withdraw_pay( delegate_account->id, required_fees.amount );
      required_signatures.insert( delegate_account->active_key() ); 

       
      if (sign)
      {
          sign_transaction( trx, required_signatures );
          my->_blockchain->store_pending_transaction( trx, true );
           // TODO: cache transaction
      }

      return trx;
   }

   signed_transaction wallet::vote_proposal( const string& delegate_name, 
                                             proposal_id_type proposal_id, 
                                             proposal_vote::vote_type vote,
                                             const string& message,
                                             bool sign )
   {
      if( !is_valid_account_name( delegate_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("delegate_name",delegate_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      // TODO validate subject, body, and data

      signed_transaction trx;
      unordered_set<address>     required_signatures;

      auto delegate_account = my->_blockchain->get_account_record( delegate_name );
      FC_ASSERT(delegate_account.valid(), "No such account: ${acct}", ("acct", delegate_account));
      FC_ASSERT(delegate_account->is_delegate());

      bool found_active_delegate = false;
      auto next_active = my->_blockchain->next_round_active_delegates();
      for( const auto& delegate_id : next_active )
      {
         if( delegate_id == delegate_account->id )
         {
            found_active_delegate = true;
            break;
         }
      }
      FC_ASSERT( found_active_delegate, "Delegate ${name} is not currently active",
                 ("name",delegate_name) );
      

      FC_ASSERT(message.size() < BTS_BLOCKCHAIN_PROPOSAL_VOTE_MESSAGE_MAX_SIZE );
      trx.vote_proposal( proposal_id, delegate_account->id, vote, message );

      auto required_fees = get_priority_fee();
      required_fees += asset( my->_blockchain->calculate_data_fee(fc::raw::pack_size(trx)), 0 );
      
      /*
      my->withdraw_to_transaction( required_fees.amount,
                                   required_fees.asset_id,
                                   get_account_public_key( account->name ),
                                   trx, required_signatures );
      */

      trx.withdraw_pay( delegate_account->id, required_fees.amount );
      required_signatures.insert( delegate_account->active_key() ); 
       
      if( sign )
      {
          sign_transaction( trx, required_signatures );
          my->_blockchain->store_pending_transaction( trx, true );
           // TODO: cache transaction
      }

      return trx;
   }

   /***
    *  @param from_account_name - the account that will fund the bid
    *  @param real_quantity - the total number of items desired (ie: 10 BTC)
    *  @param quantity_symbol - the symbol for real quantity (ie: BTC)
    *  @param price_per_unit  - the quote price (ie: $600 USD )
    *  @param quote_symbol    - the symbol of the quote price (ie: USD)
    *
    *  The total funds required by this wallet will be:
    *    
    *      real_quantity * price_per_unit 
    *
    *  @note there are two possible markets USD / BTC and BTC / USD that
    *  have an inverce price relationship.  We always assume that the
    *  quote unit is greater than the base unit (in asset_id).  
    *
    *  Because the base shares are asset id 0 (ie: XTS), then if someone issues USD
    *  it will have a higher asset id, say 20.   
    *
    *  @code
    *    if( quantity_symbol < quote_symbol ) 
    *       If your quantity_symbol is XTS then
    *         amount_withdrawn = real_quantity * price_per_unit USD
    *         price_per_unit   = price_per_unit
    *       If your quantity_symbol is USD then:
    *         amount_withdrawn = real_quantity / price_per_unit USD
    *         price_per_unit   = 1 / price_per_unit
    *  @endcode
    */
   signed_transaction wallet::cancel_market_order( const address& owner_address )
   { try {
        if( NOT is_open()     ) FC_CAPTURE_AND_THROW( wallet_closed );
        if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );

        auto order_itr = my->_wallet_db.get_market_orders().find( owner_address );
        if( order_itr == my->_wallet_db.get_market_orders().end() )
           FC_CAPTURE_AND_THROW( unknown_market_order, (owner_address) );

        auto owner_key_record = my->_wallet_db.lookup_key( owner_address );
        if( NOT owner_key_record )
           FC_CAPTURE_AND_THROW( unknown_address, (owner_address) );

        auto account_key_record = my->_wallet_db.lookup_key( owner_key_record->account_address );
        FC_ASSERT( account_key_record.valid() );

        auto from_address = owner_key_record->account_address;
        auto from_account_key = account_key_record->public_key;
        auto& to_account_key = from_account_key;

        const market_order_status& order = order_itr->second;
        asset balance = order.get_balance();

        auto required_fees = get_priority_fee();

        if( balance.amount == 0 ) FC_CAPTURE_AND_THROW( zero_amount, (order) );

        signed_transaction trx;
        unordered_set<address>     required_signatures;
        required_signatures.insert( owner_address );

        switch( order.get_type() )
        {
           case ask_order:
              trx.ask( -balance, order.order.market_index.order_price, owner_address );
              break;
           case bid_order:
              trx.bid( -balance, order.order.market_index.order_price, owner_address );
              break;
           case short_order:
              trx.short_sell( -balance, order.order.market_index.order_price, owner_address );
              break;
           case cover_order:
              FC_ASSERT( !"You cannot cancel a cover order" );
              break;
            case null_order:
              FC_ASSERT( !"Null Order Detected" );
        }

        asset deposit_amount = balance;
        if( balance.asset_id == 0 )
        {
           if( required_fees.amount < balance.amount )
           {
              deposit_amount -= required_fees;
              trx.deposit( owner_address, deposit_amount, 0 );
           }
           else
           {
              FC_CAPTURE_AND_THROW( fee_greater_than_amount, (balance)(required_fees) );
           }
        }
        else
        {
           trx.deposit( owner_address, balance, 0 );

           my->withdraw_to_transaction( required_fees.amount,
                                        0,
                                        from_address,  // get address of account
                                        trx, 
                                        required_signatures );
        }

        auto entry = ledger_entry();
        entry.from_account = owner_key_record->public_key;
        entry.to_account = to_account_key;
        entry.amount = deposit_amount;
        entry.memo = "cancel " + variant( owner_key_record->memo ).as_string();

        auto record = wallet_transaction_record();
        record.is_market = true;
        record.ledger_entries.push_back( entry );
        record.fee = required_fees;

        sign_and_cache_transaction( trx, required_signatures, record );
        return trx;
   } FC_CAPTURE_AND_RETHROW( (owner_address) ) }

   signed_transaction wallet::submit_bid(
           const string& from_account_name,
           double real_quantity, 
           const string& quantity_symbol,
           double quote_price,
           const string& quote_symbol,
           bool sign )
   { try {
       if( NOT is_open()     ) FC_CAPTURE_AND_THROW( wallet_closed );
       if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );
       if( NOT is_receive_account(from_account_name) )
          FC_CAPTURE_AND_THROW( unknown_receive_account, (from_account_name) );
       if( real_quantity <= 0 )
          FC_CAPTURE_AND_THROW( negative_bid, (real_quantity) );
       if( quote_price <= 0 )
          FC_CAPTURE_AND_THROW( invalid_price, (quote_price) );
       if( quote_symbol == quantity_symbol )
          FC_CAPTURE_AND_THROW( invalid_price, (quote_price)(quantity_symbol)(quote_symbol) );
       
       auto quote_asset_record = my->_blockchain->get_asset_record( quote_symbol );
       auto base_asset_record  = my->_blockchain->get_asset_record( quantity_symbol );

       if( NOT quote_asset_record ) 
          FC_CAPTURE_AND_THROW( unknown_asset_symbol, (quote_symbol) );
       if( NOT base_asset_record ) 
          FC_CAPTURE_AND_THROW( unknown_asset_symbol, (quantity_symbol) );

       auto from_account_key = get_account_public_key( from_account_name );
       //auto& to_account_key = from_account_key;

       if( quote_asset_record->id < base_asset_record->id )
       {
          // force user to submit an ask rather than a bid
          FC_CAPTURE_AND_THROW( invalid_market, (quote_symbol)(quantity_symbol) );
       }

       double cost = real_quantity * quote_price;

       asset cost_shares( cost *  quote_asset_record->get_precision(), quote_asset_record->id );
       asset price_shares( quote_price *  quote_asset_record->get_precision(), quote_asset_record->id );
       asset base_one_quantity( base_asset_record->get_precision(), base_asset_record->id );

       auto quote_price_shares = price_shares / base_one_quantity;
       ilog( "quote price float: ${p}", ("p",quote_price) );
       ilog( "quote price shares: ${p}", ("p",quote_price_shares) );

       auto order_key = get_new_public_key( from_account_name );
       auto order_address = order_key;

       signed_transaction trx;
       unordered_set<address>     required_signatures;
       required_signatures.insert(order_address);

       private_key_type from_private_key  = get_account_private_key( from_account_name );
       address          from_address( from_private_key.get_public_key() );

       auto required_fees = get_priority_fee();

       if( cost_shares.asset_id == 0 )
       {
          my->withdraw_to_transaction( cost_shares.amount + required_fees.amount, 
                                       0, 
                                       from_address, 
                                       trx, 
                                       required_signatures );
       }
       else
       {
          /// TODO: determine if we can pay our fees in cost.asset_id
          ///        quote_asset_record->symbol );

          my->withdraw_to_transaction( cost_shares.amount,
                                       cost_shares.asset_id,
                                       from_address, 
                                       trx, 
                                       required_signatures );
          // pay our fees in XTS
          my->withdraw_to_transaction( required_fees.amount,
                                       0,
                                       from_address, 
                                       trx, 
                                       required_signatures );
       }

       trx.bid( cost_shares, quote_price_shares, order_address );

       if( sign )
       {
           std::stringstream memo;
           memo << "buy " << real_quantity << " " << base_asset_record->symbol << " @ ";
           memo << quote_price << " " << quote_asset_record->symbol;

           auto entry = ledger_entry();
           entry.from_account = from_account_key;
           entry.to_account = order_key;
           entry.amount = cost_shares;
           entry.memo = memo.str();

           auto record = wallet_transaction_record();
           record.is_market = true;
           record.ledger_entries.push_back( entry );
           record.fee = required_fees;

           sign_and_cache_transaction( trx, required_signatures, record );

           auto key_rec = my->_wallet_db.lookup_key( order_key );
           key_rec->memo = "ORDER-" + variant( address(order_key) ).as_string().substr(3,8);
           my->_wallet_db.store_key(*key_rec);
       }
       return trx;
   } FC_CAPTURE_AND_RETHROW( (from_account_name)
                             (real_quantity)(quantity_symbol)
                             (quote_price)(quote_symbol)(sign) ) }

   signed_transaction wallet::submit_ask(
           const string& from_account_name,
           double real_quantity, 
           const string& quantity_symbol,
           double quote_price,
           const string& quote_symbol,
           bool sign )
   { try {
       if( NOT is_open()     ) FC_CAPTURE_AND_THROW( wallet_closed );
       if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );
       if( NOT is_receive_account(from_account_name) )
          FC_CAPTURE_AND_THROW( unknown_receive_account, (from_account_name) );
       if( real_quantity <= 0 )
          FC_CAPTURE_AND_THROW( negative_bid, (real_quantity) );
       if( quote_price <= 0 )
          FC_CAPTURE_AND_THROW( invalid_price, (quote_price) );
       if( quote_symbol == quantity_symbol )
          FC_CAPTURE_AND_THROW( invalid_price, (quote_price)(quantity_symbol)(quote_symbol) );
       
       auto quote_asset_record = my->_blockchain->get_asset_record( quote_symbol );
       auto base_asset_record  = my->_blockchain->get_asset_record( quantity_symbol );

       if( NOT quote_asset_record ) 
          FC_CAPTURE_AND_THROW( unknown_asset_symbol, (quote_symbol) );
       if( NOT base_asset_record ) 
          FC_CAPTURE_AND_THROW( unknown_asset_symbol, (quantity_symbol) );

       auto from_account_key = get_account_public_key( from_account_name );
       //auto& to_account_key = from_account_key;

       if( quote_asset_record->id < base_asset_record->id )
       {
          // force user to submit an bid rather than a ask
          FC_CAPTURE_AND_THROW( invalid_market, (quote_symbol)(quantity_symbol) );
       }

       double cost = real_quantity; 

       asset cost_shares( cost *  base_asset_record->get_precision(), base_asset_record->id );
       asset price_shares( quote_price *  quote_asset_record->get_precision(), quote_asset_record->id );
       asset base_one_quantity( base_asset_record->get_precision(), base_asset_record->id );

       auto quote_price_shares = price_shares / base_one_quantity;
       ilog( "quote price float: ${p}", ("p",quote_price) );
       ilog( "quote price shares: ${p}", ("p",quote_price_shares) );

       auto order_key = get_new_public_key( from_account_name );
       auto order_address = order_key;

       signed_transaction trx;
       unordered_set<address>     required_signatures;
       required_signatures.insert(order_address);

       private_key_type from_private_key  = get_account_private_key( from_account_name );
       address          from_address( from_private_key.get_public_key() );

       auto required_fees = get_priority_fee();

       if( cost_shares.asset_id == 0 )
       {
          my->withdraw_to_transaction( cost_shares.amount + required_fees.amount, 
                                       0, 
                                       from_address, 
                                       trx, 
                                       required_signatures );
       }
       else
       {
          /// TODO: determine if we can pay our fees in cost.asset_id
          ///        quote_asset_record->symbol );

          my->withdraw_to_transaction( cost_shares.amount,
                                       cost_shares.asset_id,
                                       from_address, 
                                       trx, 
                                       required_signatures );
          // pay our fees in XTS
          my->withdraw_to_transaction( required_fees.amount,
                                       0,
                                       from_address, 
                                       trx, 
                                       required_signatures );
       }

       trx.ask( cost_shares, quote_price_shares, order_address );

       if( sign )
       {
           std::stringstream memo;
           memo << "sell " << real_quantity << " " << base_asset_record->symbol << " @ ";
           memo << quote_price << " " << quote_asset_record->symbol;

           auto entry = ledger_entry();
           entry.from_account = from_account_key;
           entry.to_account = order_key;
           entry.amount = cost_shares;
           entry.memo = memo.str();

           auto record = wallet_transaction_record();
           record.is_market = true;
           record.ledger_entries.push_back( entry );
           record.fee = required_fees;

           sign_and_cache_transaction( trx, required_signatures, record );

           auto key_rec = my->_wallet_db.lookup_key( order_key );
           key_rec->memo = "ORDER-" + variant( address(order_key) ).as_string().substr(3,8);
           my->_wallet_db.store_key(*key_rec);
       }
       return trx;
   } FC_CAPTURE_AND_RETHROW( (from_account_name)
                             (real_quantity)(quantity_symbol)
                             (quote_price)(quote_symbol)(sign) ) }

   /**
    *  Short $200 USD at  $20 USD / XTS   
    *  @param real_quantity - the amount in quote units that we wish to short sell
    *  @param quote_price   - the price at which we are selling them 
    *  @param quote_symbol  - the symbol of the item being sold (shorted)
    *  @param from_account  - the account that will be providing  real_quantity / quote_price XTS to 
    *                         fund the transaction.
    */
   signed_transaction wallet::submit_short(
           const string& from_account_name,
           double real_quantity, 
           double quote_price,
           const string& quote_symbol,
           bool sign )
   { try {
       if( NOT is_open()     ) FC_CAPTURE_AND_THROW( wallet_closed );
       if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );
       if( NOT is_receive_account(from_account_name) )
          FC_CAPTURE_AND_THROW( unknown_receive_account, (from_account_name) );
       if( real_quantity <= 0 )
          FC_CAPTURE_AND_THROW( negative_bid, (real_quantity) );
       if( quote_price <= 0 )
          FC_CAPTURE_AND_THROW( invalid_price, (quote_price) );
       
       auto quote_asset_record = my->_blockchain->get_asset_record( quote_symbol );
       auto base_asset_record  = my->_blockchain->get_asset_record( asset_id_type(0) );

       if( NOT quote_asset_record ) 
          FC_CAPTURE_AND_THROW( unknown_asset_symbol, (quote_symbol) );

       auto from_account_key = get_account_public_key( from_account_name );
       //auto& to_account_key = from_account_key;

       if( quote_asset_record->id == 0 )
          FC_CAPTURE_AND_THROW( shorting_base_shares, (quote_symbol) );

       double cost = real_quantity / quote_price;
       idump( (cost)(real_quantity)(quote_price) );

       asset cost_shares( cost *  base_asset_record->get_precision(), base_asset_record->id );
       asset price_shares( quote_price *  quote_asset_record->get_precision(), quote_asset_record->id );
       asset base_one_quantity( base_asset_record->get_precision(), base_asset_record->id );

       auto quote_price_shares = price_shares / base_one_quantity;

       auto order_key = get_new_public_key( from_account_name );
       auto order_address = order_key;

       signed_transaction trx;
       unordered_set<address>     required_signatures;
       required_signatures.insert(order_address);

       private_key_type from_private_key  = get_account_private_key( from_account_name );
       address          from_address( from_private_key.get_public_key() );

       auto required_fees = get_priority_fee();

       idump( (cost_shares)(required_fees) );
       my->withdraw_to_transaction( cost_shares.amount + required_fees.amount, 
                                    0, 
                                    from_address, 
                                    trx, 
                                    required_signatures );

       // withdraw to transaction cost_share_quantity + fee
       trx.short_sell( cost_shares, quote_price_shares, order_address );

       if( sign )
       {
           std::stringstream memo;
           memo << "short " << my->_blockchain->to_pretty_asset( cost_shares )
                << " @ " << my->_blockchain->to_pretty_price( quote_price_shares );

           auto entry = ledger_entry();
           entry.from_account = from_account_key;
           entry.to_account = order_key;
           entry.amount = cost_shares;
           entry.memo = memo.str();

           auto record = wallet_transaction_record();
           record.is_market = true;
           record.ledger_entries.push_back( entry );
           record.fee = required_fees;

           sign_and_cache_transaction( trx, required_signatures, record );

           auto key_rec = my->_wallet_db.lookup_key( order_key );
           key_rec->memo = "ORDER-" + variant( address(order_key) ).as_string().substr(3,8);
           my->_wallet_db.store_key(*key_rec);
       }

       return trx;
   } FC_CAPTURE_AND_RETHROW( (from_account_name)
                             (real_quantity) (quote_price)(quote_symbol)(sign) ) }

   signed_transaction wallet::cover_short(
           const string& from_account_name,
           double real_quantity_usd,
           const string& quote_symbol,
           const address& owner_address,
           bool sign  )
   { try {
       if( NOT is_open()     ) FC_CAPTURE_AND_THROW( wallet_closed );
       if( NOT is_unlocked() ) FC_CAPTURE_AND_THROW( login_required );
       if( NOT is_receive_account(from_account_name) )
          FC_CAPTURE_AND_THROW( unknown_receive_account, (from_account_name) );
       if( real_quantity_usd < 0 ) FC_CAPTURE_AND_THROW( negative_bid, (real_quantity_usd) );

       auto     from_account_key = get_account_public_key( from_account_name );
       address  from_address( from_account_key );

       optional<market_order> order_to_cover;
       auto covers = my->_blockchain->get_market_covers( quote_symbol );
       for( const auto& order : covers )
       {
          if( owner_address == order.get_owner() )
             order_to_cover = order;
       }
       FC_ASSERT( order_to_cover.valid() );

       signed_transaction trx;
       unordered_set<address>     required_signatures;
       required_signatures.insert( order_to_cover->market_index.owner );

       auto quote_asset_record = my->_blockchain->get_asset_record( order_to_cover->market_index.order_price.quote_asset_id );
       FC_ASSERT( quote_asset_record.valid() );
       asset amount_to_cover( real_quantity_usd * quote_asset_record->precision, quote_asset_record->id );
       if( real_quantity_usd == 0 )
       {
          amount_to_cover.amount = order_to_cover->state.balance; 
       }

       my->withdraw_to_transaction( amount_to_cover.amount,
                                    amount_to_cover.asset_id, 
                                    from_address, 
                                    trx, 
                                    required_signatures );

       auto required_fees = get_priority_fee();

       trx.cover( amount_to_cover, order_to_cover->market_index );

       bool fees_paid = false;
       if( amount_to_cover.amount - order_to_cover->state.balance == 0 )
       {
          if( *order_to_cover->collateral > required_fees.amount )
          {
             slate_id_type slate_id = 0;

             auto new_slate = select_delegate_vote();
             slate_id = new_slate.id();
             
             if( slate_id && !my->_blockchain->get_delegate_slate( slate_id ) )
             {
                trx.define_delegate_slate( new_slate );
             }

             trx.deposit( order_to_cover->market_index.owner, asset( *order_to_cover->collateral - required_fees.amount), slate_id );
             fees_paid = true;
          }
          else
          {
             required_fees.amount -= *order_to_cover->collateral;
          }
       }
       if( !fees_paid )
       {
             my->withdraw_to_transaction( required_fees.amount,
                                          required_fees.asset_id, 
                                          from_address, 
                                          trx, 
                                          required_signatures );
       }

       if( sign )
       {
           auto entry = ledger_entry();
           entry.from_account = from_account_key;
           entry.to_account = get_private_key( order_to_cover->get_owner() ).get_public_key();
           entry.amount = amount_to_cover;
           entry.memo = "cover ORDER-" + variant( address( order_to_cover->get_owner() ) ).as_string().substr(3,8);

           auto record = wallet_transaction_record();
           record.is_market = true;
           record.ledger_entries.push_back( entry );
           record.fee = required_fees;

           sign_and_cache_transaction( trx, required_signatures, record );
       }
       return trx;
   } FC_CAPTURE_AND_RETHROW( (from_account_name)(real_quantity_usd)(quote_symbol)(owner_address)(sign) ) }

   void wallet::set_priority_fee( const asset& fee )
   { try {
      FC_ASSERT( is_open () );
      my->_wallet_db.set_property( default_transaction_priority_fee, variant( fee ) );
   } FC_CAPTURE_AND_RETHROW( (fee) ) }

   asset wallet::get_priority_fee()const
   { try {
      FC_ASSERT( is_open () );
      // TODO: support price conversion using price from blockchain
      const auto priority_fee = my->_wallet_db.get_property( default_transaction_priority_fee );
      if( priority_fee.is_null() ) return asset( BTS_BLOCKCHAIN_DEFAULT_PRIORITY_FEE );
      try {
         return priority_fee.as<asset>(); 
      } 
      catch ( const fc::exception& e )
      {
         wlog( "priority fee setting appears corrupted, resetting to default" );
         my->_wallet_db.set_property( default_transaction_priority_fee, fc::variant( asset( BTS_BLOCKCHAIN_DEFAULT_PRIORITY_FEE) ) );
         return asset( BTS_BLOCKCHAIN_DEFAULT_PRIORITY_FEE );
      }
   } FC_CAPTURE_AND_RETHROW() }
   
   string wallet::get_key_label( const public_key_type& key )const
   { try {
       auto acct_record = my->_wallet_db.lookup_account( key );
       if (acct_record)
       {
          // pretty_trx.from_me = my->_wallet_db.has_private_key( address( key ) );
           return acct_record->name;
       }
       else
       {
           auto registered_account = my->_blockchain->get_account_record( key );
           if( registered_account.valid() )
              return registered_account->name;
           else
           {
              auto key_rec  = my->_wallet_db.lookup_key( key );
              //wdump( (key_rec) );
              if(  key_rec )
              {
                 if( key_rec->memo )
                 {
                    return *key_rec->memo;
                 }
                 else
                 {
                    acct_record = my->_wallet_db.lookup_account( key_rec->account_address ); 
                    if( acct_record )
                       return  acct_record->name;
                 }
              }
           }
       }
       return string( key );
   } FC_CAPTURE_AND_RETHROW( (key) ) }
   
   pretty_transaction wallet::to_pretty_trx( const wallet_transaction_record& trx_rec ) const
   {
      pretty_transaction pretty_trx;

      pretty_trx.is_virtual = trx_rec.is_virtual;
      pretty_trx.is_confirmed = trx_rec.is_confirmed;
      pretty_trx.is_market = trx_rec.is_market;
      pretty_trx.is_market_cancel = !trx_rec.is_virtual && trx_rec.is_market && trx_rec.trx.is_cancel();
      pretty_trx.trx_id = trx_rec.record_id;
      pretty_trx.block_num = trx_rec.block_num;

      for( const auto& entry : trx_rec.ledger_entries )
      {
          auto pretty_entry = pretty_ledger_entry();

          if( entry.from_account.valid() )
             pretty_entry.from_account = get_key_label( *entry.from_account );
          else if( trx_rec.is_virtual && trx_rec.block_num <= 0 )
             pretty_entry.from_account = "GENESIS";
          else 
             pretty_entry.from_account = "UNKNOWN";

          if( entry.to_account.valid() )
             pretty_entry.to_account = get_key_label( *entry.to_account );
          else
             pretty_entry.to_account = "UNKNOWN";

          pretty_entry.amount = entry.amount;
          pretty_entry.memo = entry.memo;

          pretty_trx.ledger_entries.push_back( pretty_entry );
      }

      pretty_trx.fee = trx_rec.fee;
      pretty_trx.created_time = trx_rec.created_time;
      pretty_trx.received_time = trx_rec.received_time;

      if( trx_rec.is_virtual ) return pretty_trx;

      auto trx = trx_rec.trx;
      for( const auto& op : trx.operations )
      {
          switch( operation_type_enum( op.type ) )
          {
              case (withdraw_op_type):
              {
                  auto pretty_op = pretty_withdraw_op();
                  auto withdraw_op = op.as<withdraw_operation>();
                  /*
                  auto name = std::string("");
                  address owner;
                  auto balance_rec = my->_blockchain->get_balance_record( withdraw_op.balance_id );
                  if (balance_rec)
                  {
                      owner = balance_rec->owner();
                      if (owner) 
                      {
                          owallet_account_record acc_rec = my->_wallet_db.lookup_account( owner );
                          if ( acc_rec )
                              name = acc_rec->name;
                      }
                  }
                  
                  pretty_op.owner = std::make_pair(owner, name);
                  pretty_op.amount = withdraw_op.amount;
                  */
                  pretty_trx.add_operation(pretty_op);
                  break;
              }
              case (deposit_op_type):
              {
                  auto pretty_op = pretty_deposit_op();
                  auto deposit_op = op.as<deposit_operation>();
/*
                  // TODO
                  account_id_type vote = deposit_op.condition.delegate_id;
                  account_id_type pos_delegate_id = (vote > 0) ? vote : account_id_type(-vote);
                  int32_t delegate_account_num = my->_wallet_db.account_id_to_account[pos_delegate_id];
                  blockchain::oaccount_record delegate_acct_rec = my->_blockchain->get_account_record( delegate_account_num );
                  string delegate_name = delegate_acct_rec ? delegate_acct_rec->name : "";
                  pretty_op.vote = std::make_pair(vote, delegate_name);

                  if( withdraw_condition_types( deposit_op.condition.type ) == withdraw_signature_type )
                  {
                      auto name = std::string("");
                      auto condition = deposit_op.condition.as<withdraw_with_signature>();
                      auto acc_rec = my->_wallet_db.lookup_account( condition.owner );
                      if ( acc_rec )
                          name = acc_rec->name;
                      pretty_op.owner = std::make_pair( condition.owner, name );
                  }
                  else
                  {
                      FC_ASSERT(false, "Unimplemented withdraw condition: ${c}",
                                      ("c", deposit_op.condition.type));
                  }

                  pretty_op.amount = deposit_op.amount;
*/
                  pretty_trx.add_operation(pretty_op);
                  break;
              }
              case( register_account_op_type ):
              {
                  auto reserve_name_op = op.as<register_account_operation>();
                  auto pretty_op = pretty_reserve_name_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( update_account_op_type ):
              {
                  auto update_name_op = op.as<update_account_operation>();
                  auto pretty_op = pretty_update_name_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( create_asset_op_type ):
              {
                  //auto create_asset_op = op.as<create_asset_operation>();
                  auto pretty_op = pretty_create_asset_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( update_asset_op_type ):
              {
                  //auto update_asset_op = op.as<update_asset_operation>();
                  auto pretty_op = pretty_update_asset_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( issue_asset_op_type ):
              {
                  //auto issue_asset_op = op.as<issue_asset_operation>();
                  auto pretty_op = pretty_issue_asset_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( submit_proposal_op_type ):
              {
                  //auto submit_proposal_op = op.as<submit_proposal_operation>();
                  auto pretty_op = pretty_submit_proposal_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case( vote_proposal_op_type ):
              {
                  //auto vote_proposal_op = op.as<vote_proposal_operation>();
                  auto pretty_op = pretty_vote_proposal_op();
                  pretty_trx.add_operation( pretty_op );
                  break;
              }
              case withdraw_pay_op_type:
              {
                 pretty_trx.add_operation( op );
                 break;
              }
              case bid_op_type:
              {
                 pretty_trx.add_operation( op );
                 break;
              }
              default:
              {
                 pretty_trx.add_operation( op );
                 break;
              }
          } // switch op_type
      } // for op in trx
     
      return pretty_trx;
   }

   void wallet::import_bitcoin_wallet( const path& wallet_dat,
                                     const string& wallet_dat_passphrase,
                                     const string& account_name )
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto keys = bitcoin::import_bitcoin_wallet( wallet_dat, wallet_dat_passphrase );
      for( const auto& key : keys )
         import_private_key( key, account_name );

      scan_chain( 0, 1 );
      std::cout << "Successfully imported " << keys.size() << " keys from: " << wallet_dat.filename().string() << "\n";
   } FC_RETHROW_EXCEPTIONS( warn, "error importing bitcoin wallet ${wallet_dat}", 
                            ("wallet_dat",wallet_dat)("account_name",account_name) ) }

   void wallet::import_multibit_wallet( const path& wallet_dat,
                                     const string& wallet_dat_passphrase,
                                     const string& account_name )
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto keys = bitcoin::import_multibit_wallet( wallet_dat, wallet_dat_passphrase );

      for( const auto& key : keys )
         import_private_key( key, account_name );

      scan_chain( 0, 1 );
      std::cout << "Successfully imported " << keys.size() << " keys from: " << wallet_dat.filename().string() << "\n";
   } FC_RETHROW_EXCEPTIONS( warn, "error importing bitcoin wallet ${wallet_dat}", 
                            ("wallet_dat",wallet_dat)("account_name",account_name) ) }

   void wallet::import_electrum_wallet( const path& wallet_dat,
                                     const string& wallet_dat_passphrase,
                                     const string& account_name )
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto keys = bitcoin::import_electrum_wallet( wallet_dat, wallet_dat_passphrase );

      for( const auto& key : keys )
         import_private_key( key, account_name );

      scan_chain( 0, 1 );
      std::cout << "Successfully imported " << keys.size() << " keys from: " << wallet_dat.filename().string() << "\n";
   } FC_RETHROW_EXCEPTIONS( warn, "error importing bitcoin wallet ${wallet_dat}", 
                            ("wallet_dat",wallet_dat)("account_name",account_name) ) }

   void wallet::import_armory_wallet( const path& wallet_dat,
                                     const string& wallet_dat_passphrase,
                                     const string& account_name )
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );

      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );

      auto keys = bitcoin::import_armory_wallet( wallet_dat, wallet_dat_passphrase );

      for( const auto& key : keys )
         import_private_key( key, account_name );

      scan_chain( 0, 1 );
      std::cout << "Successfully imported " << keys.size() << " keys from: " << wallet_dat.filename().string() << "\n";
   } FC_RETHROW_EXCEPTIONS( warn, "error importing bitcoin wallet ${wallet_dat}", 
                            ("wallet_dat",wallet_dat)("account_name",account_name) ) }
    
    void wallet::import_keyhotee( const std::string& firstname,
                                 const std::string& middlename,
                                 const std::string& lastname,
                                 const std::string& brainkey,
                                 const std::string& keyhoteeid )
    { try {
      if( !is_valid_account_name( fc::to_lower( keyhoteeid ) ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid Keyhotee name!", ("keyhoteeid",keyhoteeid) );

        FC_ASSERT( is_open() );
        FC_ASSERT( is_unlocked() );
        // TODO: what will keyhoteeid's validation be like, they have different rules?
        
        bts::keyhotee::profile_config config{firstname, middlename, lastname, brainkey};
        
        auto private_key = bts::keyhotee::import_keyhotee_id(config, keyhoteeid);
        
        import_private_key(private_key, fc::to_lower(keyhoteeid), true);
        
        scan_chain( 0, 1 );
        std::cout << "Successfully imported Keyhotee private key.\n";
    } FC_RETHROW_EXCEPTIONS( warn, "error creating private key using keyhotee info.",
                            ("firstname",firstname)("middlename",middlename)("lastname",lastname)("brainkey",brainkey)("keyhoteeid",keyhoteeid) ) }

   vector<string> wallet::list() const
   {
       vector<string> wallets;
       if (!fc::is_directory(get_data_directory()))
           return wallets;

       auto path = get_data_directory();
       fc::directory_iterator end_itr; // constructs terminator
       for( fc::directory_iterator itr( path ); itr != end_itr; ++itr)
       {
          if (fc::is_directory( *itr ))
          {
              wallets.push_back( (*itr).stem().string() );
          }
       }

       std::sort( wallets.begin(), wallets.end() );
       return wallets;
   }

   bool  wallet::is_sending_address( const address& addr )const
   { try {
      return !is_receive_address( addr );
   } FC_CAPTURE_AND_RETHROW() }


   bool  wallet::is_receive_address( const address& addr )const
   {  try {
      auto key_rec = my->_wallet_db.lookup_key( addr );
      if( key_rec.valid() )
         return key_rec->has_private_key();
      return false;
   } FC_CAPTURE_AND_RETHROW() }

   vector<wallet_account_record> wallet::list_accounts() const
   { try {
      const auto& accs = my->_wallet_db.get_accounts();

      vector<wallet_account_record> accounts;
      accounts.reserve( accs.size() );
      for( const auto& item : accs )
      {
         FC_ASSERT(item.second.is_my_account == my->_wallet_db.has_private_key( item.second.account_address )
                 , "\'is_my_account\' field fell out of sync" );
         accounts.push_back( item.second );
      }

      std::sort( accounts.begin(), accounts.end(),
                 [](const wallet_account_record& a, const wallet_account_record& b) -> bool
                 { return a.name.compare( b.name ) < 0; } );

      return accounts;
   } FC_CAPTURE_AND_RETHROW() }

   vector<wallet_account_record> wallet::list_my_accounts() const
   { try {
      const auto& accs = my->_wallet_db.get_accounts();

      vector<wallet_account_record> receive_accounts;
      receive_accounts.reserve( accs.size() );
      for( const auto& item : accs )
      {
         if ( my->_wallet_db.has_private_key( item.second.account_address ) )
         {
            receive_accounts.push_back( item.second );
         }
      }

      std::sort( receive_accounts.begin(), receive_accounts.end(),
                 [](const wallet_account_record& a, const wallet_account_record& b) -> bool
                 { return a.name.compare( b.name ) < 0; } );

      return receive_accounts;
   } FC_CAPTURE_AND_RETHROW() }


   vector<wallet_account_record> wallet::list_favorite_accounts() const
   { try {
      const auto& accs = my->_wallet_db.get_accounts();

      vector<wallet_account_record> receive_accounts;
      receive_accounts.reserve( accs.size() );
      for( const auto& item : accs )
      {
         if( item.second.is_favorite )
         {
            receive_accounts.push_back( item.second );
         }
      }

      std::sort( receive_accounts.begin(), receive_accounts.end(),
                 [](const wallet_account_record& a, const wallet_account_record& b) -> bool
                 { return a.name.compare( b.name ) < 0; } );

      return receive_accounts;
   } FC_CAPTURE_AND_RETHROW() }

   vector<wallet_account_record> wallet::list_unregistered_accounts() const
   { try {
      const auto& accs = my->_wallet_db.get_accounts();

      vector<wallet_account_record> receive_accounts;
      receive_accounts.reserve( accs.size() );
      for( const auto& item : accs )
      {
         if( item.second.id == 0 )
         {
            receive_accounts.push_back( item.second );
         }
      }

      std::sort( receive_accounts.begin(), receive_accounts.end(),
                 [](const wallet_account_record& a, const wallet_account_record& b) -> bool
                 { return a.name.compare( b.name ) < 0; } );

      return receive_accounts;
   } FC_CAPTURE_AND_RETHROW() }

   owallet_transaction_record wallet::lookup_transaction( const transaction_id_type& trx_id )const
   {
       return my->_wallet_db.lookup_transaction(trx_id);
   }

   vector<wallet_transaction_record> wallet::get_pending_transactions()const
   {
       return my->get_pending_transactions();
   }

   void wallet::hide_pending_transactions()
   {
      my->hide_pending_transactions();
      auto tmp_balances = my->_wallet_db.get_balances();
      for( const auto& item : tmp_balances )
         my->sync_balance_with_blockchain( item.first );
   }

   map<transaction_id_type, fc::exception> wallet::get_pending_transaction_errors()const
   { try {
       map<transaction_id_type, fc::exception> transaction_errors;
       const auto& transaction_records = get_pending_transactions();
       const auto priority_fee = my->_blockchain->get_priority_fee();
       for( const auto& transaction_record : transaction_records )
       {
           FC_ASSERT( !transaction_record.is_virtual && !transaction_record.is_confirmed );
           const auto error = my->_blockchain->get_transaction_error( transaction_record.trx, priority_fee );
           if( !error.valid() ) continue;
           transaction_errors[ transaction_record.trx.id() ] = *error;
       }
       return transaction_errors;
   } FC_CAPTURE_AND_RETHROW() }

   void wallet::scan_state( const time_point_sec& received_time )
   { try {
      ilog( "WALLET: Scanning blockchain state" );
      my->scan_balances( received_time );
      my->scan_registered_accounts();
   } FC_RETHROW_EXCEPTIONS( warn, "" )  }

   /**
    *  A valid account is any named account registered in the blockchain or
    *  any local named account.
    */
   bool wallet::is_valid_account( const string& account_name )const
   {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );
      FC_ASSERT( is_open() );
      if( my->_wallet_db.lookup_account( account_name ).valid() )
          return true;
      return my->_blockchain->get_account_record( account_name ).valid();
   }

   /**
    *  Any account for which this wallet owns the private key.
    */
   bool wallet::is_receive_account( const string& account_name )const
   {
      FC_ASSERT( is_open() );
      if( !is_valid_account_name( account_name ) ) return false;
      auto opt_account = my->_wallet_db.lookup_account( account_name );
      if( !opt_account.valid() ) return false;
      auto opt_key = my->_wallet_db.lookup_key( opt_account->account_address );
      if( !opt_key.valid()  ) return false;
      return opt_key->has_private_key();
   }

   /**
    * Account names are limited the same way as domain names.
    */
   bool wallet::is_valid_account_name( const string& account_name )const
   {
      return my->_blockchain->is_valid_account_name( account_name );
   }

   private_key_type wallet::get_account_private_key( const string& account_name )const
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );
      FC_ASSERT( is_open() );
      FC_ASSERT( is_unlocked() );
      auto opt_account = my->_wallet_db.lookup_account( account_name );
      FC_ASSERT( opt_account.valid(), "Unable to find account '${name}'", 
                ("name",account_name) );

      auto opt_key = my->_wallet_db.lookup_key( opt_account->account_address );
      FC_ASSERT( opt_key.valid(), "Unable to find key for account '${name}", 
                ("name",account_name) );

      FC_ASSERT( opt_key->has_private_key() );
      return opt_key->decrypt_private_key( my->_wallet_password );
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   bool wallet::is_unique_account( const string& account_name )const
   {
      //There are two possibilities here. First, the wallet has multiple records named account_name
      //Second, the wallet has a different record named account_name than the blockchain does.

      //Check that the wallet has at most one account named account_name
      auto known_accounts = my->_wallet_db.get_accounts();
      bool found = false;
      for( const auto& known_account : known_accounts )
      {
        if( known_account.second.name == account_name )
        {
          if( found ) return false;
          found = true;
        }
      }

      if( !found )
        //The wallet does not contain an account with this name. No conflict is possible.
        return true;

      //The wallet has an account named account_name. Check that it matches with the blockchain
      auto local_account      = my->_wallet_db.lookup_account( account_name );
      auto registered_account = my->_blockchain->get_account_record( account_name );
      if( local_account && registered_account )
         return local_account->account_address == address( registered_account->active_key() );
      return local_account || registered_account;
   }

   /**
    *  Looks up the public key for an account whether local or in the blockchain, with
    *  the local name taking priority.
    */
   public_key_type wallet::get_account_public_key( const string& account_name )const
   { try {
      if( !is_valid_account_name( account_name ) )
          FC_THROW_EXCEPTION( invalid_name, "Invalid account name!", ("account_name",account_name) );
      FC_ASSERT( is_open() );

      auto opt_account = my->_wallet_db.lookup_account( account_name );
      if( !opt_account.valid() )
      {
         auto registered_account = my->_blockchain->get_account_record( account_name );
         if( registered_account.valid() )
            return registered_account->active_key();
      }
      FC_ASSERT( opt_account.valid(), "Unable to find account '${name}'", 
                ("name",account_name) );

      auto opt_key = my->_wallet_db.lookup_key( opt_account->account_address );
      FC_ASSERT( opt_key.valid(), "Unable to find key for account '${name}", 
                ("name",account_name) );

      return opt_key->public_key;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("account_name",account_name) ) }

   /**
    *  Randomly select a slate of BTS_BLOCKCHAIN_MAX_SLATE_SIZE delegates from those approved
    *  by this wallet. The slate will be no more than BTS_BLOCKCHAIN_NUM_DELEGATES.
    */
   delegate_slate wallet::select_delegate_vote()const
   {
      FC_ASSERT( BTS_BLOCKCHAIN_MAX_SLATE_SIZE <= BTS_BLOCKCHAIN_NUM_DELEGATES );
      vector<account_id_type> for_candidates;

      for( const auto& acct_rec : my->_wallet_db.get_accounts() )
      {
         if( acct_rec.second.approved )
             for_candidates.push_back( acct_rec.second.id );
      }
      std::random_shuffle( for_candidates.begin(), for_candidates.end() );

      auto slate = delegate_slate();
      if( for_candidates.size() <= BTS_BLOCKCHAIN_MAX_SLATE_SIZE )
          slate.supported_delegates = for_candidates;
      else
          slate.supported_delegates = vector<account_id_type>( for_candidates.begin(), for_candidates.begin() + BTS_BLOCKCHAIN_MAX_SLATE_SIZE );

      FC_ASSERT( slate.supported_delegates.size() <= BTS_BLOCKCHAIN_MAX_SLATE_SIZE );
      std::sort( slate.supported_delegates.begin(), slate.supported_delegates.end() );
      return slate;
   }

   void wallet::set_delegate_approval( const string& delegate_name, int approved )
   { try {
      FC_ASSERT( is_open() );
      auto war = my->_wallet_db.lookup_account( delegate_name );
      if( war.valid() )
      {
         war->approved = approved;
         my->_wallet_db.cache_account( *war );
      }
      else
      {
         auto reg_account = my->_blockchain->get_account_record( delegate_name );
         if( !reg_account.valid() )
         {
            FC_ASSERT( !"Not a Registered Account" );
         }
         add_contact_account( delegate_name, reg_account->active_key() );
         set_delegate_approval( delegate_name, approved );
      }
   } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name",delegate_name)("approved", approved) ) }

   int wallet::get_delegate_approval( const string& delegate_name )const
   { try {
      FC_ASSERT( is_open() );
      auto war = my->_wallet_db.lookup_account( delegate_name );
      FC_ASSERT( war.valid() );
      return war->approved;
   } FC_RETHROW_EXCEPTIONS( warn, "", ("delegate_name",delegate_name) ) }

   owallet_account_record wallet::get_account_record( const address& addr)const
   {
      return my->_wallet_db.lookup_account( addr );
   }

   owallet_account_record  wallet::get_account_for_address( address addr_in_account )
   {
       auto okey = my->_wallet_db.lookup_key( addr_in_account );
       if (! okey.valid() )
           return owallet_account_record();
       return get_account_record( okey->account_address );
   }

   account_vote_summary_type wallet::get_account_vote_summary( const string& account_name )const
   {
      unordered_map<account_id_type, int64_t> raw_votes;
      for( const auto& b : my->_wallet_db.get_balances() )
      {
          auto okey_rec = my->_wallet_db.lookup_key( b.second.owner() );
          if( okey_rec && okey_rec->has_private_key() )
          {
             auto oacct_rec = my->_wallet_db.lookup_account( okey_rec->account_address );
             if ( !(account_name == "" || (oacct_rec.valid() && oacct_rec->name == account_name)) )
                 continue;

             asset bal = b.second.get_balance();
             if( bal.asset_id == 0 )
             {
                if( b.second.delegate_slate_id() != 0 )
                {
                    odelegate_slate slate = my->_blockchain->get_delegate_slate(b.second.delegate_slate_id());
                    FC_ASSERT( slate.valid() );
                    for( const auto& delegate_id : slate->supported_delegates )
                    {
                        if( raw_votes.count( delegate_id ) <= 0 )
                            raw_votes[ delegate_id ] = bal.amount;
                        else
                            raw_votes[ delegate_id ] += bal.amount;
                    }
                }
             }
          }
      }
      account_vote_summary_type result;
      for( const auto& item : raw_votes )
      {
         auto delegate_account = my->_blockchain->get_account_record( item.first );
         result[delegate_account->name] = item.second;
      }
      return result;
   }

   account_balance_summary_type wallet::get_account_balances( const string& account_name )const
   { try {
      FC_ASSERT( is_open() );
      if( !account_name.empty() ) get_account( account_name ); /* Just to check input */

      const auto pending_state = my->_blockchain->get_pending_state();
      auto raw_results = map<address, std::pair<map<asset_id_type, share_type>, share_type>>();
      auto result = account_balance_summary_type();

      for( const auto& b : my->_wallet_db.get_balances() )
      {
          const auto okey_rec = my->_wallet_db.lookup_key( b.second.owner() );
          if( !okey_rec.valid() || !okey_rec->has_private_key() ) continue;
          const auto account_address = okey_rec->account_address;

          const auto obalance = pending_state->get_balance_record( b.first );
          auto balance = asset( 0 );
          if( obalance.valid() )
              balance = obalance->get_balance();

          /* Simpler to just check every time */
          const auto oaccount = pending_state->get_account_record( account_address );
          auto pay_balance = share_type( 0 );
          if( oaccount.valid() && oaccount->is_delegate() )
              pay_balance = oaccount->delegate_info->pay_balance;

          if( balance.amount <= 0 && pay_balance <= 0 ) continue;

          if( raw_results.count( account_address ) <= 0 )
              raw_results[ account_address ] = std::make_pair( map<asset_id_type, share_type>(), share_type( 0 ) );

          if( raw_results[ account_address ].first.count( balance.asset_id ) <= 0 )
              raw_results[ account_address ].first[ balance.asset_id ] = balance.amount;
          else
              raw_results[ account_address ].first[ balance.asset_id ] += balance.amount;

          raw_results[ account_address ].second = pay_balance;
      }

      for( const auto& account : raw_results )
      {
         const auto oaccount = my->_wallet_db.lookup_account( account.first );
         const auto name = oaccount.valid() ? oaccount->name : string( account.first );
         if( !account_name.empty() && name != account_name ) continue;

         if( result.count( name ) <= 0 )
             result[ name ] = std::make_pair( map<string, share_type>(), share_type( 0 ) );

         for( const auto& item : account.second.first )
         {
            const auto symbol = my->_blockchain->get_asset_symbol( item.first );
            result[ name ].first[ symbol ] = item.second;
         }

         result[ name ].second = account.second.second;
      }

      return result;
   } FC_RETHROW_EXCEPTIONS(warn,"") }

   variant wallet::get_info()const
   {
       fc::mutable_variant_object obj;
       obj( "data_directory", fc::absolute(my->_data_directory) );
       if( is_open() )
       {
          obj( "last_unlocked_scanned_block_number", my->_wallet_db.get_property( last_unlocked_scanned_block_number ) );
          obj( "last_locked_scanned_block_number", my->_wallet_db.get_property( last_locked_scanned_block_number ) );
          obj( "next_child_key_index", my->_wallet_db.get_property( next_child_key_index ) );
          obj( "default_transaction_priority_fee", get_priority_fee() );
          obj( "state", "open" );
          obj( "locked", is_locked() );
          obj( "file", fc::absolute(my->_current_wallet_path) );
          fc::optional<fc::time_point_sec> relock_time_in_sec;
          if (my->_scheduled_lock_time)
            relock_time_in_sec = *my->_scheduled_lock_time;                                
          obj( "scheduled_lock_time", relock_time_in_sec);
          obj( "scan_progress", my->_scan_progress );
       }
       else
       {
          obj( "state", "closed" );
       }
       return obj;
   }

   public_key_summary wallet::get_public_key_summary( const public_key_type& pubkey ) const
   {
       public_key_summary summary;
       summary.hex = variant( fc::ecc::public_key_data(pubkey) ).as_string();
       summary.native_pubkey = string( pubkey );
       summary.native_address = string( address( pubkey ) );
       summary.pts_normal_address = string( pts_address( pubkey, false, 56 ) );
       summary.pts_compressed_address = string( pts_address( pubkey, true, 56 ) );
       summary.btc_normal_address = string( pts_address( pubkey, false, 0 ) );
       summary.btc_compressed_address = string( pts_address( pubkey, true, 0 ) );
       return summary;
   }
   
   vector<public_key_type> wallet::get_public_keys_in_account( const string& account_name )const
   {
      public_key_type  account_public_key = get_account_public_key( account_name );
      address          account_address( account_public_key );

      vector<public_key_type> account_keys;
      for( const auto& key : my->_wallet_db.get_keys() )
      {
         if( key.second.account_address == account_address || key.first == account_address )
            account_keys.push_back( key.second.public_key );
      }
      return account_keys;
   }

   vector<market_order>  wallet::get_market_orders( const string& quote_symbol, const string& base_symbol )const
   { try {
      auto bids   = my->_blockchain->get_market_bids( quote_symbol, base_symbol );
      auto asks   = my->_blockchain->get_market_asks( quote_symbol, base_symbol );
      auto shorts = my->_blockchain->get_market_shorts( quote_symbol );
      auto covers = my->_blockchain->get_market_covers( quote_symbol );

      vector<market_order> result;

      for( const auto& order : bids )
      {
         if( my->_wallet_db.has_private_key( order.get_owner() ) )
            result.push_back( order );
      }

      for( const auto& order : asks )
      {
         if( my->_wallet_db.has_private_key( order.get_owner() ) )
            result.push_back( order );
      }

      for( const auto& order : shorts )
      {
         if( my->_wallet_db.has_private_key( order.get_owner() ) )
            result.push_back( order );
      }

      for( const auto& order : covers )
      {
         if( my->_wallet_db.has_private_key( order.get_owner() ) )
            result.push_back( order );
      }
      return result;
   } FC_CAPTURE_AND_RETHROW( (quote_symbol)(base_symbol) ) }

} } // bts::wallet
