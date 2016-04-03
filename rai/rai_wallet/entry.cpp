#include <rai/qt/qt.hpp>

#include <rai/node/working.hpp>
#include <rai/icon.hpp>
#include <rai/node/rpc.hpp>

#include <boost/make_shared.hpp>

#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

class qt_wallet_config
{
public:
	qt_wallet_config () :
	account (0),
	rpc_enable (false)
	{
		rai::random_pool.GenerateBlock (wallet.bytes.data (), wallet.bytes.size ());
		assert (!wallet.is_zero ());
	}
	bool upgrade_json (unsigned version_a, boost::property_tree::ptree & tree_a)
	{
		auto result (false);
		switch (version_a)
		{
		case 1:
		{
			rai::account account;
			account.decode_account (tree_a.get <std::string> ("account"));
			tree_a.erase ("account");
			tree_a.put ("account", account.to_account ());
			tree_a.erase ("version");
			tree_a.put ("version", "2");
			result = true;
		}
		case 2:
		{
			boost::property_tree::ptree rpc_l;
			rpc.serialize_json (rpc_l);
			tree_a.put ("rpc_enable", "false");
			tree_a.put_child ("rpc", rpc_l);
			tree_a.erase ("version");
			tree_a.put ("version", "3");
			result = true;
		}
		case 3:
		break;
		default:
		throw std::runtime_error ("Unknown qt_wallet_config version");
		}
		return result;
	}
	bool deserialize_json (bool & upgraded_a, boost::property_tree::ptree & tree_a)
	{
		auto error (false);
		if (!tree_a.empty ())
		{
			auto version_l (tree_a.get_optional <std::string> ("version"));
			if (!version_l)
			{
				tree_a.put ("version", "1");
				version_l = "1";
				upgraded_a = true;
			}
			upgraded_a |= upgrade_json (std::stoull (version_l.get ()), tree_a);
			auto wallet_l (tree_a.get <std::string> ("wallet"));
			auto account_l (tree_a.get <std::string> ("account"));
			auto & node_l (tree_a.get_child ("node"));
			rpc_enable = tree_a.get <bool> ("rpc_enable");
			auto & rpc_l (tree_a.get_child ("rpc"));
			try
			{
				error |= wallet.decode_hex (wallet_l);
				error |= account.decode_account (account_l);
				error |= node.deserialize_json (upgraded_a, node_l);
				error |= rpc.deserialize_json (rpc_l);
				if (wallet.is_zero ())
				{
					rai::random_pool.GenerateBlock (wallet.bytes.data (), wallet.bytes.size ());
					upgraded_a = true;
				}
			}
			catch (std::logic_error const &)
			{
				error = true;
			}
		}
		else
		{
			serialize_json (tree_a);
			upgraded_a = true;
		}
		return error;
	}
	void serialize_json (boost::property_tree::ptree & tree_a)
	{
		std::string wallet_string;
		wallet.encode_hex (wallet_string);
		tree_a.put ("version", "3");
		tree_a.put ("wallet", wallet_string);
		tree_a.put ("account", account.to_account ());
		boost::property_tree::ptree node_l;
		node.serialize_json (node_l);
		tree_a.add_child ("node", node_l);
		boost::property_tree::ptree rpc_l;
		rpc.serialize_json (rpc_l);
		tree_a.add_child ("rpc", rpc_l);
		tree_a.put ("rpc_enable", rpc_enable);
	}
	bool serialize_json_stream (std::ostream & stream_a)
	{
		auto result (false);
		stream_a.seekp (0);
		try
		{
			boost::property_tree::ptree tree;
			serialize_json (tree);
			boost::property_tree::write_json (stream_a, tree);
		}
		catch (std::runtime_error const &)
		{
			result = true;
		}
		return result;
	}
	rai::uint256_union wallet;
	rai::account account;
	rai::node_config node;
	bool rpc_enable;
	rai::rpc_config rpc;
};

int run_wallet (int argc, char * const * argv)
{
	auto working (rai::working_path ());
	boost::filesystem::create_directories (working);
	qt_wallet_config config;
	auto config_path ((working / "config.json").string ());
	std::fstream config_file;
	rai::open_or_create (config_file, config_path);
    int result (0);
    if (!config_file.fail ())
    {
		auto error (rai::fetch_object (config, config_file));
		if (!error)
		{
			QApplication application (argc, const_cast <char **> (argv));
			rai::set_application_icon (application);
			auto service (boost::make_shared <boost::asio::io_service> ());
			rai::work_pool work (config.node.opencl_work);
			rai::alarm alarm (*service);
			rai::node_init init;
			auto node (std::make_shared <rai::node> (init, *service, working, alarm, config.node, work));
			auto pool (boost::make_shared <boost::network::utils::thread_pool> (node->config.io_threads));
			if (!init.error ())
			{
				if (config.account.is_zero ())
				{
					auto wallet (node->wallets.create (config.wallet));
					config.account = wallet->deterministic_insert ();
					assert (wallet->exists (config.account));
					error = config.serialize_json_stream (config_file);
				}
				if (!error)
				{
					auto wallet (node->wallets.open (config.wallet));
					if (wallet != nullptr)
					{
						if (wallet->exists (config.account))
						{
							node->start ();
							rai::rpc rpc (service, pool, *node, config.rpc);
							if (config.rpc_enable)
							{
								rpc.start ();
							}
							std::unique_ptr <rai_qt::wallet> gui (new rai_qt::wallet (application, *node, wallet, config.account));
							gui->client_window->show ();
							rai::thread_runner runner (*service, node->config.io_threads);
							QObject::connect (&application, &QApplication::aboutToQuit, [&] ()
							{
								rpc.stop ();
								node->stop ();
							});
							try
							{
								result = application.exec ();
							}
							catch (...)
							{
								result = -1;
								assert (false);
							}
							runner.join ();
							error = config.serialize_json_stream (config_file);
						}
						else
						{
							std::cerr << "Wallet account doesn't exist\n";
						}
					}
					else
					{
						std::cerr << "Wallet id doesn't exist\n";
					}
				}
				else
				{
					std::cerr << "Error writing config file\n";
				}
			}
			else
			{
				std::cerr << "Error initializing node\n";
			}
		}
		else
		{
			std::cerr << "Error deserializing config\n";
		}
    }
    else
    {
        std::cerr << "Unable to open config file\n";
    }
	return result;
}

int main (int argc, char * const * argv)
{
	boost::program_options::options_description description ("Command line options");
	description.add_options () ("help", "Print out options");
	rai::add_node_options (description);
	boost::program_options::variables_map vm;
	boost::program_options::store (boost::program_options::command_line_parser (argc, argv).options (description).allow_unregistered ().run (), vm);
	boost::program_options::notify (vm);
	int result (0);
	if (!rai::handle_node_options (vm))
	{
	}
	else if (vm.count ("help") != 0)
	{
		std::cout << description << std::endl;
	}
    else
    {
		result = run_wallet (argc, argv);
    }
    return result;
}
